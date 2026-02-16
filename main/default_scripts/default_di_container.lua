local M = { providers = {}, bindings = {}, singletons = {} }

function M.reset()
    M.providers = {}
    M.bindings = {}
    M.singletons = {}
end

function M.provide(name, factory)
    assert(type(name) == 'string' and name ~= '', 'bad provider name')
    assert(type(factory) == 'function', 'factory must be function')
    M.providers[name] = factory
end

function M.bind(iface, provider, opts)
    assert(type(iface) == 'string' and iface ~= '', 'bad interface')
    assert(type(provider) == 'string' and provider ~= '', 'bad provider')
    M.bindings[iface] = { provider = provider, opts = opts or {} }
    M.singletons[iface] = nil
end

function M.resolve(iface)
    if M.singletons[iface] then
        return M.singletons[iface]
    end
    local b = assert(M.bindings[iface], 'no binding: ' .. iface)
    local f = assert(M.providers[b.provider], 'no provider: ' .. b.provider)
    local obj = f(b.opts, M)
    M.singletons[iface] = obj
    return obj
end

return M
