// Test element.dataset proxy

const el = document.createElement('div');
document.getElementById('root').appendChild(el);

// Set via setAttribute, read via dataset
el.setAttribute('data-name', 'alice');
assert(el.dataset.name === 'alice', 'dataset reads data-name');

// Set via dataset, read via getAttribute
el.dataset.age = '30';
assert(el.getAttribute('data-age') === '30', 'dataset sets data-age attribute');

// Hyphenated names: data-foo-bar -> dataset.fooBar
el.setAttribute('data-foo-bar', 'baz');
assert(el.dataset.fooBar === 'baz', 'hyphenated data attr becomes camelCase');

// Set camelCase via dataset -> becomes hyphenated attribute
el.dataset.longName = 'value';
assert(el.getAttribute('data-long-name') === 'value', 'camelCase dataset becomes hyphenated attr');

// Cleanup
document.getElementById('root').innerHTML = '';
