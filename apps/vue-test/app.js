try {
    const { createApp, ref, computed } = Vue;

    const app = createApp({
        template: `
            <div>
                <h1>{{ message }}</h1>
                <button id="btn" @click="increment">Count: {{ count }}</button>
                <p v-if="count > 2">Count is greater than 2!</p>
                <ul>
                    <li v-for="item in items" :key="item">{{ item }}</li>
                </ul>
                <p>Double count: {{ doubleCount }}</p>
            </div>
        `,
        setup() {
            const message = ref('Hello Vue!');
            const count = ref(0);
            const items = ref(['Apple', 'Banana', 'Cherry']);

            const doubleCount = computed(() => count.value * 2);

            function increment() {
                count.value++;
                console.log('Count: ' + count.value + ', Double: ' + doubleCount.value);
            }

            globalThis.increment = increment;
            globalThis.addItem = function(item) {
                items.value.push(item);
            };

            return { message, count, items, doubleCount, increment };
        }
    });

    app.mount('#app');
    console.log('Vue app mounted!');
} catch(e) {
    console.log('Error:', e.message);
    console.log('Stack:', e.stack);
}
