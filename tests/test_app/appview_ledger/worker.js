// A heartbeat the page can watch from outside: the time, written to a file
// every 20 ms until the app's worker is stopped.
const fs = require('fs');
self.onmessage = (e) => {
  const path = e.data;
  setInterval(() => fs.writeFileSync(path, String(Date.now())), 20);
  self.postMessage('beating');
};
