-- Test brasslua running in bro's shared realm with DOM, bro globals, and JS interop

-- 1. True 64-bit integer vs float subtypes in Lua 5.4
local big_int = (1 << 60) + 777
assert(math.type(big_int) == "integer", "64-bit integer subtype")
assert(math.type(3.14) == "float", "float subtype")
assert(big_int - 777 == (1 << 60), "exact 64-bit integer arithmetic")

-- 2. DOM element creation, attributes, and tree mutation via method calls (`obj:m(...)`)
local div = document:createElement("div")
assert(div ~= nil and div ~= null, "document:createElement returns an element")
assert(div.tagName == "DIV", "div.tagName is DIV")
assert(div.nodeType == 1, "div.nodeType is 1")
assert(div.parentNode == null, "detached element parentNode is JS null sentinel")
assert(div.parentElement == null, "detached element parentElement is JS null sentinel")

div:setAttribute("id", "brasslua-test-node")
div:setAttribute("data-count", "42")
assert(div:getAttribute("id") == "brasslua-test-node", "getAttribute returns id")
assert(div:getAttribute("data-count") == "42", "getAttribute returns data-count")
assert(div:getAttribute("nonexistent") == null, "missing attribute returns JS null sentinel")

local span = document:createElement("span")
span.textContent = "Hello from brasslua!"
div:appendChild(span)
document.body:appendChild(div)

local found = document:getElementById("brasslua-test-node")
assert(found ~= null and found ~= nil, "getElementById finds appended element")
assert(found.textContent == "Hello from brasslua!", "element textContent matches")
assert(found.children.length == 1, "element has 1 child")

-- 3. Event listener callback from DOM to Lua closure on shared brass::gc::Heap
local click_count = 0
local last_event_type = ""
div:addEventListener("click", function(ev)
    click_count = click_count + 10
    last_event_type = ev.type
end)
div:click()
assert(click_count == 10, "Lua closure event listener invoked on DOM click")
assert(last_event_type == "click", "DOM event object passed to Lua closure")

-- 4. Plain table conversion to JS Array (1-based sequence) and JS Object (record)
local json_arr = JSON.stringify({ 10, 20, 30, 40 })
assert(json_arr == "[10,20,30,40]", "1-based Lua sequence converts to 0-based JS Array")

local parsed = totable(JSON.parse('{"engine":"bro","lang":"lua","items":[5,6,7]}'))
assert(parsed.engine == "bro", "JSON.parse object field engine")
assert(parsed.lang == "lua", "JSON.parse object field lang")
assert(#parsed.items == 3 and parsed.items[1] == 5 and parsed.items[3] == 7, "JSON.parse array converted to 1-based Lua table")

-- 5. BigInt round-trip across JS boundary for integers past 2^53
local big_fn = BigInt
local js_bigint = big_fn("1152921504606847753")
assert(math.type(js_bigint) == "integer", "JS BigInt converts to Lua 64-bit integer")
assert(js_bigint == big_int, "JS BigInt value matches (1 << 60) + 777")

-- 6. Promise + coroutine yield/resume via await()
local p = Promise:resolve(1234)
local resolved_val = await(p)
assert(resolved_val == 1234, "await(Promise:resolve(1234)) returns 1234")

-- 7. Shared realm globals between Lua and JavaScript
luaGlobalAdd = function(a, b)
    return a + b
end
local js_call_res = eval("globalThis.luaGlobalAdd(19, 23)")
assert(js_call_res == 42, "JS eval can call Lua global function in shared realm")

eval("globalThis.jsGlobalMul = (x, y) => x * y;")
assert(jsGlobalMul(6, 9) == 54, "Lua can call JS global function defined in shared realm")

-- Verify Lua sees page state initialized by index.html's JS <script>
assert(window.__lifecycle ~= nil and window.__lifecycle ~= null, "Lua reads window.__lifecycle set by page JS")
assert(window.__lifecycle.load.readyState == "complete", "Lua reads nested JS object properties")


