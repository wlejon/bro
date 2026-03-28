var btn = document.getElementById("btn");
var counter = document.getElementById("counter");
var count = 0;

btn.addEventListener("click", function() {
    count++;
    counter.textContent = "Count: " + count;
    console.log("Clicked:", count);
});

console.log("Scene demo loaded!");
