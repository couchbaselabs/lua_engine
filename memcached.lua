print("hello world")

function memcached_get(key)
  print("memcached_get " .. key)
  return 0
end

function memcached_store(key, operation)
  print("memcached_store " .. key .. " " .. operation)
  return 0
end

function memcached_remove(key)
  print("memcached_remove " .. key)
  return 0
end

function memcached_flush(when)
  print("memcached_flush " .. when)
  return 0
end

