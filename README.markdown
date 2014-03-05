# Couchbase Lua Engine

## Building

You will need a development environment for Couchbase, in particular the
headers from memcached.

### Building the Lua Engine

    cd ~/prog
    git clone git@github.com:northscale/lua_engine.git
    cd lua_engine
    ./config/autorun.sh
    ./configure --with-memcached=$HOME/prog/memcached
    make

## Running

TODO: document how to plug this in with Couchbase.

Against memcached directly, here is an example invocation:

    ~/prog/memcached/memcached -v \
       -E ~/prog/lua_engine/.libs/lua_engine.so
