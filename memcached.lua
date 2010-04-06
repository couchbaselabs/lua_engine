print("hello world")

dict = {}

function memcached_get(key)
  local v = dict[key]
  if v then
    local val = v[1]
    local flg = v[2]
    local exp = v[3]
    print("memcached_get " .. key .. " " .. val .. " " .. flg .. " " .. exp)
    return val, flg, exp
  else
    print("memcached_get " .. key .. " [miss]")
    return nil, 0, 0
  end
end

function memcached_store(key, operation, val, flg, exp)
  print("memcached_store " .. key .. " " .. operation .. " " .. val .. " " .. flg .. " " .. exp)
  dict[key] = {val, flg, exp}
  return 0
end

function memcached_remove(key)
  print("memcached_remove " .. key)
  dict[key] = nil
  return 0
end

function memcached_flush(when)
  print("memcached_flush " .. when)
  dict = {}
  return 0
end

