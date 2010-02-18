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
    data public servers::array
    data public max_key_length::integer
    data public max_value_length::integer
    data public server_timeout::integer = 5
    
    data private _connection = net_tcp 
    
    public onCreate(servers::array, -max_key_length::integer=250, -max_value_length::integer=1024 * 1024) => {
        .'servers' = #servers
        (#max_key_length ? .'max_key_length' = #max_key_length)
        (#max_value_length ? .'max_value_length' = #max_value_length)
    }
    
    private MAX_TTL_OFFSET => 60 * 60 * 24 * 30 //number of seconds in 30 days
    private DEFAULT_TTL => 60 * 60
    
    
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
    public cas() => {}
    
    // Retriaval Commands
    public get(key::string) => {return ._retrievalCommand('get', #key)}
    public gets(key::string) => {return ._retrievalCommand('gets', #key)}
    
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
        /* Pointless to ever issue this command (UNLESS WE CACHE THE CONNECTION (NEVER CLOSE))
        From the Docs:
        "quit" is a command with no arguments:  quit\r\n

        Upon receiving this command, the server closes the connection. 
        However, the client may also simply close the connection when it no longer needs it, 
        without issuing this command.*/
    }
    //public stats() => {}
    //public version() => {}
    
    
    private _serverHash(key::string) => {
        return bytes(#key)->crc
    }
    private _findServer(key::string) => {
        local(server_index) = (._serverHash(#key) % .'servers'->size) + 1
        local(result) = .'servers'->get(#server_index)->split(':')
        #result = pair(#result->get(1), integer(#result->get(2)))
        return #result
    }
    
    private _storeCommand(command::string, key::string, value::string, expires::integer, flags::integer) => {
        local(server_pair) = ._findServer(#key)
        local(comm) = #command + ' ' + #key + ' ' + #flags + ' ' + #expires  + ' ' + bytes(#value)->size + '\r\n'
        .'_connection' = net_tcp
        .'_connection'->connect(#server_pair->first, #server_pair->second, .'server_timeout')
        .'_connection'->writeBytes(bytes(#comm))
        .'_connection'->writeBytes(bytes(#value + "\r\n"))
        local(result) = string(.'_connection'->readSomeBytes(1024, 5))
        .'_connection'->close
        
        return #result
    }
    
    /***
        This does the heavy-lifting of geting values out of memcached
        Returns a map if there is a single key passed, otherwise an array of maps
    ***/
    private _retrievalCommand(command::string, key::string) => {
        /*
        *  Maximum # of bytes per key retrieved = 6 + max_key_length+1 + 11 + sizeOf(max_value_length)+1 + 20 + 2 + max_value_length + 2
        *  After all the items have been transmitted, the server sends the string "END\r\n"
        *  So last part is + 5 for total_max_bytes
        */
        local(num_keys) = #key->split(' ')->size
        local(total_max_bytes) = #num_keys * (6 + .'max_key_length'+1 + 11 + .'max_value_length'->asString()->size+1 + 20 + 2 + .'max_value_length' + 2) + 5
        local(result) = array
        local(server_pair) = ._findServer(#key)
        local(comm) = #command + ' ' + #key + '\r\n'
        .'_connection' = net_tcp
        .'_connection'->connect(#server_pair->first, #server_pair->second, .'server_timeout')
        .'_connection'->writeBytes(bytes(#comm))
        local(blob) = .'_connection'->readSomeBytes(#total_max_bytes, 5)
        .'_connection'->close
        
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
        return (#result->size == 1 ? #result->get(1) | (#result->size == 0 ? void | #result))
    }
    
    private _otherCommands(key::string, command::string, read_bytes::integer=0) => {
        local(result)
        local(server_pair) = ._findServer(#key)
        local(comm) = #command + '\r\n'
        .'_connection' = net_tcp
        .'_connection'->connect(#server_pair->first, #server_pair->second, .'server_timeout')
        .'_connection'->writeBytes(bytes(#comm))
        #result = (#read_bytes > 0 ? string(.'_connection'->readSomeBytes(#read_bytes, .'server_timeout')) | null)
        .'_connection'->close
        return #result
    }
    
    private _allServersCommand(command::string) => {
        local(comm) = #command + '\r\n'
        iterate(.'servers', local(server)) => {
            .'_connection' = net_tcp
            .'_connection'->connect(#server->split(':')->get(1), integer(#server->split(':')->get(2)))
            .'_connection'->writeBytes(bytes(#comm))
            .'_connection'->close
        }
    }
}