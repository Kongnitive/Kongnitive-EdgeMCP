#!/bin/bash
# Download Lua 5.4.7 source for ESP32 component
# Run from wss_server/components/lua/

LUA_VERSION="5.4.7"
LUA_URL="https://www.lua.org/ftp/lua-${LUA_VERSION}.tar.gz"

echo "Downloading Lua ${LUA_VERSION}..."
curl -L -o lua.tar.gz "${LUA_URL}"

echo "Extracting..."
tar xzf lua.tar.gz

echo "Copying source files..."
cp lua-${LUA_VERSION}/src/*.c src/
cp lua-${LUA_VERSION}/src/*.h src/

# Remove standalone programs (we embed Lua, not run it standalone)
rm -f src/lua.c src/luac.c

echo "Cleaning up..."
rm -rf lua-${LUA_VERSION} lua.tar.gz

echo "Done! Lua ${LUA_VERSION} source is in src/"
ls src/*.c | wc -l
echo "C files ready for build"
