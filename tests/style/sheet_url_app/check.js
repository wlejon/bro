// Run by tests/style/test_sheet_relative_urls.js in a child bro-headless.
const bg = (id) => getComputedStyle(document.getElementById(id)).backgroundImage;
const base = location.href.replace(/[^/]*$/, '');
const want = {
    linked: `url("${base}css/img/blue.png")`, quoted: `url("${base}css/img/blue.png")`, media: `url("${base}css/img/blue.png")`,
    imported: `url("${base}css/img/red.png")`, inline: `url("${base}css/img/red.png")`,
};
for (const [id, v] of Object.entries(want)) assert(bg(id) === v, `${id}: ${bg(id)} (want ${v})`);
assert(bg('data').startsWith('url("data:image/gif;base64,'), 'data: url kept: ' + bg('data'));
flush(); advanceTime(50); flush(); sleep(50); flush(); advanceTime(50); flush();
const f = frameImage();
const px = (id) => { const r = document.getElementById(id).getBoundingClientRect(); const i = ((r.top + 4) * f.width + 4) * 4; return [f.data[i], f.data[i + 1], f.data[i + 2]].join(','); };
for (const id of ['linked', 'quoted', 'media']) assert(px(id) === '0,0,255', `${id} painted blue: ${px(id)}`);
for (const id of ['imported', 'inline']) assert(px(id) === '255,0,0', `${id} painted red: ${px(id)}`);
// The computed URL is loadable as it stands (Leaflet sets it as an img src),
// as is an <img>'s own absolute src copied to another.
const img = new Image();
img.src = bg('linked').slice(5, -2);
const copy = new Image();
copy.src = document.createElement('img').src = img.src;
for (let i = 0; i < 10 && !(img.complete && copy.complete); i++) { flush(); advanceTime(20); flush(); sleep(10); }
assert(img.naturalWidth === 8, 'a bro://app/ url loads as an image: ' + img.src + ' -> ' + img.naturalWidth);
assert(copy.naturalWidth === 8, 'a copied absolute src loads: ' + copy.naturalWidth);
console.log('sheet urls: ok');
