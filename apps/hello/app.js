let count = 0;
const btn = document.getElementById("btn");
const counter = document.getElementById("counter");
const message = document.getElementById("message");

btn.addEventListener("click", function() {
    count++;
    counter.textContent = "Count: " + count;

    if (count === 1) {
        message.textContent = "You clicked once!";
    } else {
        message.textContent = "You clicked " + count + " times!";
    }

    console.log("Button clicked, count:", count);
});

console.log("Hello app loaded!");
