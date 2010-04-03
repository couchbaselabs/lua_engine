# Memcached Lua Engine

## Building

You will need a storage-engine capable memcached and its included
headers.

The easiest way to do this if you don't want to install memcached from
source would be to just create a source tree and reference it.

### Building Memcached

For example, assume you keep all of your projects in `~/prog/`, you
can do this:

    cd ~/prog
    git clone git://github.com/dustin/memcached.git
    cd memcached
    git checkout --track -b engine origin/engine
    ./config/autorun.sh
    ./configure
    make

### Building the Lua Engine

    cd ~/prog
    git clone git@github.com:northscale/lua_engine.git
    cd lua_engine
    ./config/autorun.sh
    ./configure --with-memcached=$HOME/prog/memcached
    make

## Running

An example invocation:

    ~/prog/memcached/memcached -v \
       -E ~/prog/lua_engine/.libs/lua_engine.so
