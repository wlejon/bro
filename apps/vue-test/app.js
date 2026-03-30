try {
    var Vue3 = Vue;
    var createApp = Vue3.createApp;
    var ref = Vue3.ref;
    var reactive = Vue3.reactive;
    var computed = Vue3.computed;
    var watch = Vue3.watch;
    var watchEffect = Vue3.watchEffect;
    var onMounted = Vue3.onMounted;
    var provide = Vue3.provide;
    var inject = Vue3.inject;

    // --- Child Components ---

    var CounterButton = {
        props: ['label', 'count'],
        emits: ['increment'],
        template: '<button @click="$emit(\'increment\')">{{ label }}: {{ count }}</button>'
    };

    var ThemeDisplay = {
        setup: function() {
            var theme = inject('theme', 'unknown');
            return { theme: theme };
        },
        template: '<span class="tag">Theme: {{ theme }}</span>'
    };

    var TabA = { template: '<div>Tab A — <b>reactive data</b></div>' };
    var TabB = { template: '<div>Tab B — <i>component switching</i></div>' };
    var TabC = { template: '<div>Tab C — <span class="highlight">dynamic!</span></div>' };

    // --- Main App ---

    var app = createApp({
        components: {
            CounterButton: CounterButton,
            ThemeDisplay: ThemeDisplay
        },

        setup: function() {
            var counter = ref(0);
            var message = ref('Hello Vue 3!');
            var showPanel = ref(true);
            var selectedColor = ref('#38bdf8');
            var barWidth = ref(50);

            var state = reactive({
                firstName: 'John',
                lastName: 'Doe',
                todos: [
                    { id: 1, text: 'Test reactivity', done: true },
                    { id: 2, text: 'Test directives', done: false },
                    { id: 3, text: 'Test transitions', done: false },
                    { id: 4, text: 'Test components', done: false }
                ],
                nextId: 5,
                newTodo: '',
                logs: []
            });

            var fullName = computed(function() { return state.firstName + ' ' + state.lastName; });
            var doubleCount = computed(function() { return counter.value * 2; });
            var doneCount = computed(function() {
                var n = 0;
                for (var i = 0; i < state.todos.length; i++) if (state.todos[i].done) n++;
                return n;
            });
            var todoStats = computed(function() { return doneCount.value + '/' + state.todos.length; });
            var reversedMsg = computed(function() { return message.value.split('').reverse().join(''); });

            watch(counter, function(n, o) { addLog('Counter: ' + o + ' -> ' + n); });

            function addLog(msg) {
                state.logs.unshift(msg);
                if (state.logs.length > 6) state.logs.pop();
            }

            function increment() { counter.value++; }
            function decrement() { if (counter.value > 0) counter.value--; }
            function resetCounter() { counter.value = 0; addLog('Reset'); }

            function addTodo() {
                var text = state.newTodo;
                if (text && text.trim) text = text.trim();
                if (!text) return;
                state.todos.push({ id: state.nextId++, text: text, done: false });
                state.newTodo = '';
            }
            function removeTodo(id) {
                for (var i = 0; i < state.todos.length; i++) {
                    if (state.todos[i].id === id) { state.todos.splice(i, 1); break; }
                }
            }
            function toggleTodo(id) {
                for (var i = 0; i < state.todos.length; i++) {
                    if (state.todos[i].id === id) { state.todos[i].done = !state.todos[i].done; break; }
                }
            }

            var tabs = { TabA: TabA, TabB: TabB, TabC: TabC };
            var activeTab = ref('TabA');

            var theme = ref('dark');
            provide('theme', theme);
            function toggleTheme() {
                theme.value = theme.value === 'dark' ? 'light' : 'dark';
                addLog('Theme: ' + theme.value);
            }

            onMounted(function() { addLog('App mounted'); });

            globalThis.vm = {
                counter: counter, state: state, increment: increment,
                addTodo: addTodo, showPanel: showPanel, activeTab: activeTab,
                theme: theme, toggleTheme: toggleTheme
            };

            return {
                counter: counter, message: message, showPanel: showPanel,
                selectedColor: selectedColor, barWidth: barWidth,
                state: state, fullName: fullName, doubleCount: doubleCount,
                doneCount: doneCount, todoStats: todoStats, reversedMsg: reversedMsg,
                increment: increment, decrement: decrement, resetCounter: resetCounter,
                addTodo: addTodo, removeTodo: removeTodo, toggleTodo: toggleTodo,
                tabs: tabs, activeTab: activeTab,
                theme: theme, toggleTheme: toggleTheme, addLog: addLog
            };
        },

        template: '<h1>Vue 3 Feature Test</h1>'
            + '<div class="grid">'

            // Card 1: Reactivity
            + '<div class="card"><h3>Reactivity + Events</h3>'
            +   '<div style="margin-bottom:8px">'
            +     '<CounterButton label="Count" v-bind:count="counter" v-on:increment="increment" /> '
            +     '<button @click="decrement">-1</button> '
            +     '<button @click="resetCounter" class="danger">Reset</button>'
            +   '</div>'
            +   '<div class="mono">counter={{ counter }}, double={{ doubleCount }}</div>'
            + '</div>'

            // Card 2: v-model
            + '<div class="card"><h3>v-model / Two-way Binding</h3>'
            +   '<input v-model="message" />'
            +   '<div style="margin-top:4px">Message: <b>{{ message }}</b></div>'
            +   '<div class="muted">Reversed: {{ reversedMsg }}</div>'
            +   '<div style="margin-top:6px">'
            +     '<input v-model="state.firstName" />'
            +     '<input v-model="state.lastName" />'
            +     '<div>Full name: <b>{{ fullName }}</b></div>'
            +   '</div>'
            + '</div>'

            // Card 3: v-if/v-show
            + '<div class="card"><h3>v-if / v-show / v-else</h3>'
            +   '<button @click="showPanel = !showPanel">Toggle ({{ showPanel ? "visible" : "hidden" }})</button>'
            +   '<div style="margin-top:6px">'
            +     '<div v-if="counter === 0" class="muted">Counter is zero</div>'
            +     '<div v-else-if="counter < 5" style="color:#fbbf24">Counter &lt; 5</div>'
            +     '<div v-else style="color:#22c55e">Counter &gt;= 5</div>'
            +   '</div>'
            +   '<div v-if="showPanel" style="margin-top:6px;padding:8px;background:#334155;border-radius:4px">'
            +     'Conditional panel (v-if)'
            +   '</div>'
            +   '<div v-show="counter % 2 === 0" class="muted" style="margin-top:4px">'
            +     '(v-show: visible when even)'
            +   '</div>'
            + '</div>'

            // Card 4: v-for / Todo
            + '<div class="card"><h3>v-for / Todo List</h3>'
            +   '<div style="margin-bottom:6px">'
            +     '<input v-model="state.newTodo" placeholder="New todo..." />'
            +     '<button @click="addTodo" class="primary" style="margin-top:4px">Add</button> '
            +     '<span class="tag">{{ todoStats }}</span>'
            +   '</div>'
            +   '<div v-for="todo in state.todos" v-bind:key="todo.id" style="padding:4px 0;display:flex">'
            +     '<span @click="toggleTodo(todo.id)"'
            +       ' v-bind:style="{ textDecoration: todo.done ? \'line-through\' : \'none\','
            +       ' color: todo.done ? \'#64748b\' : \'#e2e8f0\', cursor: \'pointer\', flex: \'1\' }">'
            +       '{{ todo.done ? "[x]" : "[ ]" }} {{ todo.text }}</span>'
            +     '<button @click="removeTodo(todo.id)" class="danger" style="padding:1px 6px;font-size:11px">x</button>'
            +   '</div>'
            + '</div>'

            // Card 5: Dynamic styles
            + '<div class="card"><h3>Dynamic Styles + Classes</h3>'
            +   '<div v-bind:style="{ color: selectedColor, fontWeight: \'bold\', fontSize: \'16px\' }">Styled text</div>'
            +   '<div style="margin-top:6px">'
            +     '<button v-for="c in [\'#38bdf8\',\'#22c55e\',\'#f59e0b\',\'#ef4444\',\'#a78bfa\']" v-bind:key="c"'
            +       ' @click="selectedColor = c"'
            +       ' v-bind:style="{ background: c, width: \'28px\', height: \'28px\','
            +       ' border: selectedColor === c ? \'2px solid white\' : \'2px solid transparent\','
            +       ' borderRadius: \'50%\', padding: \'0\' }"></button>'
            +   '</div>'
            +   '<div style="margin-top:8px" v-bind:class="{ highlight: counter > 3, error: counter > 7 }">'
            +     '{{ counter > 7 ? "error!" : counter > 3 ? "highlighted" : "normal" }}'
            +   '</div>'
            +   '<div style="margin-top:6px">'
            +     '<div v-bind:style="{ width: barWidth + \'%\', height: \'12px\', background: \'#3b82f6\', borderRadius: \'6px\' }"></div>'
            +     '<div class="mono" style="margin-top:2px">width: {{ barWidth }}%</div>'
            +     '<button @click="barWidth = Math.max(0, barWidth - 10)">-10%</button> '
            +     '<button @click="barWidth = Math.min(100, barWidth + 10)">+10%</button>'
            +   '</div>'
            + '</div>'

            // Card 6: Dynamic components + provide/inject
            + '<div class="card"><h3>Components + Provide/Inject</h3>'
            +   '<div style="margin-bottom:6px">'
            +     '<button v-for="(comp, name) in tabs" v-bind:key="name" @click="activeTab = name"'
            +       ' v-bind:style="{ background: activeTab === name ? \'#2563eb\' : \'#334155\' }">{{ name }}</button>'
            +   '</div>'
            +   '<component v-bind:is="tabs[activeTab]" />'
            +   '<div style="margin-top:8px"><ThemeDisplay /> '
            +     '<button @click="toggleTheme" style="margin-left:6px">Toggle Theme</button>'
            +   '</div>'
            + '</div>'

            // Card 7: Event log
            + '<div class="card"><h3>Watch / Event Log</h3>'
            +   '<div v-if="state.logs.length === 0" class="muted">No events yet.</div>'
            +   '<div v-for="(msg, i) in state.logs" v-bind:key="i" class="mono" style="padding:2px 0;color:#94a3b8">{{ msg }}</div>'
            + '</div>'

            + '</div>'
    });

    app.mount('#app');
    console.log('Vue app mounted successfully');

} catch(e) {
    console.log('Vue Error: ' + e.message);
    if (e.stack) console.log('Stack: ' + e.stack);
}
