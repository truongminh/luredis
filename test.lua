local redis_endpoint = string.format("%s:%d","localhost",6379)
local redis_queue = "queue"
local key = "foo"

local send = luredis.send
local tstring = luredis.string -- the "string" type

-- this version support one internal buffer
-- use the internal buffer as an implicit input
-- this function increase performance for large buffer
-- for safeness call clrbuf to clear buffer before use
-- max buffer length can be found in luredis.h

local clrbuf = luredis.clrbuf -- clear the internal buffer
local appbuf = luredis.appbuf -- append data to the buffer
local conbuf = luredis.conbuf -- using seperator between appended data. Like "1;2;3;4".
local sendbuf = luredis.sendbuf -- send buffer with the last argument is implicitly the internal buffer

print(luredis._VERSION,"\n",luredis._COPYRIGHT,"\n",luredis._DESCRIPTION,"\n")

-- SEND DATA TO REDIS SERVER
local content,rtype = send(redis_endpoint,"set","foo","bar")
if rtype ~= tstring then -- return type is similar to that of redis
    print("Send Error")
end

-- MANAPULATE DATA WITH BUFFER
clrbuf()
appbuf("1","2",3,4,nil,6,7,"foo") -- supported types are string, number and nil
-- SEND DATA IN BUFFER
local content,rtype = sendbuf(redis_endpoint,"lpush",redis_queue) -- the implicit argument is the internal buffer
if rtype ~= tstring then -- return type is similar to that of redis
    -- we may store the internal buffer for latter retry
    print("sendbuf with BUFFER:", luredis.getbuf())
end

-- GET DATA FROM REDIS SERVER
local content, rtype = send(redis_endpoint,"hget",redis_queue,key);
if rtype == tstring then
    print(content)
end
