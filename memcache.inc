/*
Copyright 2010 Bradley Lindsay

   Licensed under the Apache License, Version 2.0 (the "License");
   you may not use this file except in compliance with the License.
   You may obtain a copy of the License at

       http://www.apache.org/licenses/LICENSE-2.0

   Unless required by applicable law or agreed to in writing, software
   distributed under the License is distributed on an "AS IS" BASIS,
   WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
   See the License for the specific language governing permissions and
   limitations under the License.
*/

/***
    Extending the bytes type so that I can give it a maximum split value
    (Code is adapted from Lasso 9 source)
***/
define bytes->split(find::bytes, max::integer) => {
    local(num_splits = 0)
	local(ret=array)
	if (#find->size == 0) // empty string. split each char
		with c in 1 to self->size
		take #max
		do #ret->insert(self->sub(#c, 1))
		#ret->insert(self->sub(#max + 1))
	else
		local(prevFndPos=1, fndPos = self->find(#find))
		#fndPos != 0?
			{
				#ret->insert(self->sub(#prevFndPos, #fndPos - #prevFndPos))
				#prevFndPos = #fndPos + #find->length;
				#num_splits++
				#fndPos = (#num_splits < #max ? self->find(#find, #prevFndPos) | 0);
				#fndPos != 0?
					currentCapture->restart
			}()
		if (#prevFndPos <= .size)
			#ret->insert(self->sub(#prevFndPos))
		else (#prevFndPos > .size)
			#ret->insert(bytes)
		/if
	/if;
	return(#ret);
}


define memcache => type {
    private MAX_TTL_OFFSET => 60 * 60 * 24 * 30 //number of seconds in 30 days
    private MAX_KEY_LENGTH => 250
    private MAX_VALUE_LENGTH => 1024 * 1024
    
    data private servers::array
    data public server_timeout::integer
    data public DEFAULT_TTL = 60 * 60
    
    public onCreate(servers::array, -server_timeout::integer=5) => {
        .'servers' = array
        .'server_timeout' = #server_timeout
        with server in #servers
        do {
            local(conn) = net_tcp
            local(ip) = #server->split(':')->get(1)
            local(port) = integer(#server->split(':')->get(2))
            #conn->connect(#ip, #port, .'server_timeout')
            .'servers'->insert(map('ip'=#ip, 'port'=#port, 'conn'=#conn))
        }
    }
    
    // Storage Commands
    public set(key::string, value::string, -expires::integer=.DEFAULT_TTL, -flags::integer=1) => {
        ._storeCommand('set', #key, #value, #expires, #flags)
    }
    public add(key::string, value::string, -expires::integer=.DEFAULT_TTL, -flags::integer=1) => {
        ._storeCommand('add', #key, #value, #expires, #flags)
    }
    public replace(key::string, value::string, -expires::integer=.DEFAULT_TTL, -flags::integer=1) => {
        ._storeCommand('replace', #key, #value, #expires, #flags)
    }
    public append(key::string, value::string, -expires::integer=.DEFAULT_TTL, -flags::integer=1) => {
        ._storeCommand('append', #key, #value, #expires, #flags)
    }
    public prepend(key::string, value::string, -expires::integer=.DEFAULT_TTL, -flags::integer=1) => {
        ._storeCommand('prepend', #key, #value, #expires, #flags)
    }
    public cas(key::string, value::string, cas::integer, -expires::integer=.DEFAULT_TTL, -flags::integer=1) => {
        ._storeCommand('cas', #key, #value, #expires, #flags, #cas)
    }
    
    // Retriaval Commands
    public get(key::string) => {return ._retrievalCommand('get', array(#key))}
    public gets(key::string) => {return ._retrievalCommand('gets', array(#key))}
    public get(keys::array) => {return ._retrievalCommand('get', #keys)}
    public gets(keys::array) => {return ._retrievalCommand('gets', #keys)}
    
    // Other Data Commands
    public delete(key::string, time::integer=0) => {
        ._otherCommands(#key, 'delete ' + #key + ' ' + #time, 12)
    }
    public incr(key::string, amount::integer=1) => {
        ._otherCommands(#key, 'incr ' + #key + ' ' + #amount, 22)
    }
    public decr(key::string, amount::integer=1) => {
        ._otherCommands(#key, 'decr ' + #key + ' ' + #amount, 22)
    }
    
    // Commands for all servers.
    public flush_all(delay::integer=0) => {._allServersCommand('flush_all ' + #delay + ' noreply')}
    public verbosity(level::integer) => {._allServersCommand('verbosity ' + #level + ' noreply')}
    public quit() => {
        /* Pointless to ever issue this command to the server
        From the Docs:
        "quit" is a command with no arguments:  quit\r\n

        Upon receiving this command, the server closes the connection. 
        However, the client may also simply close the connection when it no longer needs it, 
        without issuing this command.*/
        
        // So we'll just close the connections
        with server in .'servers'
        do {
            #server->find('conn')->close
            #server->find('conn') = null
        }
    }
    public version() => {
        return ._allServersCommand('version', 20)
    }
    //public stats() => {}
    
    
    /**
        Provides Key Validation.
            - Key must be non-zero length
            - Key must be ASCII
            - Key must not contain a space or control characters
    **/
    private _validateKey(key::string) => {
        
        fail_if((#key->size < 1), error_code_invalidParameter, error_msg_invalidParameter)
        
        with c in #key->split('')
        do {
            fail_if((bytes(#c)->export8bits < 33 || bytes(#c)->export8bits > 126), 
                error_code_invalidParameter, error_msg_invalidParameter)
        }
    }
    
    private _serverHash(key::string) => {
        return bytes(#key)->crc
    }
    private _findServer(key::string) => {
        return (._serverHash(#key) % .'servers'->size) + 1
    }
    
    private _storeCommand(command::string, key::string, value::string, expires::integer, flags::integer, cas=null) => {
        ._validateKey(#key)
        local(server_index) = ._findServer(#key)
        local(comm) = #command + ' ' + #key + ' ' + #flags + ' ' + #expires  + ' ' + bytes(#value)->size
        if(#command == 'cas') => {
            fail_if(!#cas->isA(::integer), error_code_invalidParameter, error_msg_invalidParameter)
            #comm += ' ' + #cas
        }
        #comm += '\r\n'
        if(.'servers'->get(#server_index)->find('conn') == null) => {
            .'servers'->get(#server_index)->find('conn') = net_tcp
            .'servers'->get(#server_index)->find('conn')->connect(.'servers'->get(#server_index)->find('ip'), .'servers'->get(#server_index)->find('port'), .'server_timeout')            
        }
        local(conn) = .'servers'->get(#server_index)->find('conn')
        #conn->writeBytes(bytes(#comm))
        #conn->writeBytes(bytes(#value + "\r\n"))
        local(result) = string(#conn->readSomeBytes(1024, 5))
        
        return #result
    }
    
    
    /***
        This does the heavy-lifting of geting values out of memcached
        Returns a map if there is a single key passed, otherwise an array of maps
    ***/
    private _retrievalCommand(command::string, keys::array) => {
        local(result) = array
        local(servers) = map
        with key in #keys
        do {
            ._validateKey(#key)
            local(server_index) = ._findServer(#key)
            if(#servers->find(.'servers'->get(#server_index)->find('ip'))) => {
                #servers->find(.'servers'->get(#server_index)->find('ip'))->find('keys')->insert(#key)
            else
                #servers->insert(.'servers'->get(#server_index)->find('ip')=map('index'=#server_index, 'keys'=array(#key)))
            }
        }
        
        with server in #servers
        do {
            local(comm) = #command + ' ' + #server->find('keys')->join(' ') + '\r\n'
            local(num_keys) = #server->find('keys')->size
            if(.'servers'->get(#server->find('index'))->find('conn') == null) => {
                .'servers'->get(#server->find('index'))->find('conn') = net_tcp
                .'servers'->get(#server->find('index'))->find('conn')->connect(.'servers'->get(#server->find('index'))->find('ip'), .'servers'->get(#server->find('index'))->find('port'), .'server_timeout')            
            }
            local(conn) = .'servers'->get(#server->find('index'))->find('conn')
            // Maximum # of bytes per key retrieved = 6 + MAX_KEY_LENGTH+1 + 11 + sizeOf(MAX_VALUE_LENGTH)+1 + 20 + 2 + MAX_VALUE_LENGTH + 2
            // After all the items have been transmitted, the server sends the string "END\r\n"
            // So last part is + 5 for total_max_bytes
            local(total_max_bytes) = #num_keys * (6 + .MAX_KEY_LENGTH+1 + 11 + .MAX_VALUE_LENGTH->asString()->size+1 + 20 + 2 + .MAX_VALUE_LENGTH + 2) + 5
            
            #conn->writeBytes(bytes(#comm))
            local(blob) = #conn->readSomeBytes(#total_max_bytes, 5)
            
            // Deal with the results
            loop(#num_keys) => {
                local(tmp_result) = map
                local(value_size)
                #blob = #blob->split(bytes('\r\n'), 1)
                local(line) = #blob->get(1)->split(' ')
                #blob = #blob->get(2)

                if(#line->get(1) == bytes('END\r\n')) => {
                    loop_abort
                else(#line->get(1) != 'VALUE')
                    //TODO: Raise an error for unexpected happenstance
                    loop_abort
                }
                #tmp_result->insert('key'=string(#line->get(2)))
                #tmp_result->insert('flags'=integer(#line->get(3)))
                #value_size = integer(#line->get(4))
                #tmp_result->insert('cas'=(#line->size >= 5 ? integer(#line->get(5)) | void))
                #tmp_result->insert('value'=string(#blob->sub(1, #value_size)))
                #blob = #blob->sub(#value_size + 3)
                #result->insert(#tmp_result)
            }
        }
        
        return (#result->size == 1 ? #result->get(1) | (#result->size == 0 ? void | #result))
    }
    
    private _otherCommands(key::string, command::string, read_bytes::integer=0) => {
        ._validateKey(#key)
        local(result)
        if(.'servers'->get(._findServer(#key))->find('conn') == null) => {
            .'servers'->get(._findServer(#key))->find('conn') = net_tcp
            .'servers'->get(._findServer(#key))->find('conn')->connect(.'servers'->get(._findServer(#key))->find('ip'), .'servers'->get(._findServer(#key))->find('port'), .'server_timeout')            
        }
        local(conn) = .'servers'->get(._findServer(#key))->find('conn')
        local(comm) = #command + '\r\n'
        
        #conn->writeBytes(bytes(#comm))
        #result = (#read_bytes > 0 ? string(#conn->readSomeBytes(#read_bytes, .'server_timeout')) | null)
        
        return #result
    }
    
    private _allServersCommand(command::string, read_bytes::integer=0) => {
        local(result) = array
        local(comm) = #command + '\r\n'
        iterate(.'servers', local(server)) => {
            if(#server->find('conn') == null) => {
                #server->find('conn') = net_tcp
                #server->find('conn')->connect(#server->find('ip'), #server->find('port'), .'server_timeout')            
            }
            local(conn) = #server->find('conn')
            #conn->writeBytes(bytes(#comm))
            if(#read_bytes > 0) => {
                #result->insert(string(#conn->readSomeBytes(#read_bytes, .'server_timeout')))
            }
        }
        (#read_bytes > 0 ? return #result)
    }
}