local P = {}

local function cmd(addr, value)
    i2c.write(addr, 0x00, value)
end

local function set_pos(addr, col, page)
    cmd(addr, 0xB0 | (page & 0x07))
    cmd(addr, col & 0x0F)
    cmd(addr, 0x10 | ((col >> 4) & 0x0F))
end

local function send_page(addr, byte_value)
    local data = {0x40}
    for i = 1, 128 do
        data[i + 1] = byte_value
    end
    i2c.write(addr, data)
end

function P.factory(opts, _container)
    opts = opts or {}
    local addr = opts.addr or 0x3C
    local sda = opts.sda or 5
    local scl = opts.scl or 6
    local freq = opts.freq or 400000

    local o = {}

    function o:init()
        i2c.setup(sda, scl, freq)
        local init_seq = {
            0xAE,0xA8,0x3F,0xD3,0x00,0x40,0xA1,0xC8,0xDA,0x12,
            0x81,0xCF,0xA4,0xA6,0xD5,0x80,0xD9,0xF1,0xDB,0x40,
            0x8D,0x14,0x20,0x00,0xAF
        }
        for _, v in ipairs(init_seq) do
            cmd(addr, v)
        end
        o:clear()
    end

    function o:clear()
        for page = 0, 7 do
            set_pos(addr, 0, page)
            send_page(addr, 0x00)
        end
    end

    function o:fill(on)
        local value = on and 0xFF or 0x00
        for page = 0, 7 do
            set_pos(addr, 0, page)
            send_page(addr, value)
        end
    end

    function o:test_pattern(step)
        step = step or 0
        for page = 0, 7 do
            set_pos(addr, 0, page)
            local data = {0x40}
            for col = 0, 127 do
                local val = ((col + page + step) % 2 == 0) and 0xAA or 0x55
                data[col + 2] = val
            end
            i2c.write(addr, data)
        end
    end

    return o
end

return P
