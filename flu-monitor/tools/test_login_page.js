// Exercise the actual inline firmware login-page script without a browser.
const fs = require('node:fs'), vm = require('node:vm'), assert = require('node:assert/strict');
const source = fs.readFileSync('main/rest_api.c', 'utf8').split('static const char login_page[] = ')[1].split('\n\nstatic esp_err_t auth_status_handler')[0];
const html = [...source.matchAll(/"(?:[^"\\]|\\.)*"/g)].map(m => JSON.parse(m[0])).join('');
assert.match(html, /type="password"/);
const script = html.match(/<script>([\s\S]*?)<\/script>/)[1];
async function page(configured, remote = false) {
  const elements = Object.fromEntries(['p','c','intro','label','b','e','show','title'].map(k => [k, { value: '', style: {}, textContent: '', disabled: k === 'b', type:'password', setAttribute(name,value){this[name]=value} }]));
  const form = {}; let writes = [], redirects = [];
  const context = { AbortController, setTimeout, clearTimeout, document: { querySelector: selector => selector === 'form' ? form : elements.e, getElementById: id => elements[id] },
    location: { protocol: 'https:', hostname: 'demo.pagekite.me', host: 'demo.pagekite.me', replace: value => redirects.push(value) },
    fetch: async (url, options) => { if (!options.method) return {ok:true,json: async () => ({ configured, setup_local_only: remote })}; writes.push({url, body: JSON.parse(options.body)}); return {ok:true, json: async()=>({success:true})}; }
  };
  vm.createContext(context); vm.runInContext(script, context);
  await new Promise(resolve => setImmediate(resolve));
  return { context, elements, form, writes, redirects };
}
(async () => {
  const setup = await page(false);
  assert.equal(setup.elements.b.disabled, false);
  setup.elements.show.onclick(); assert.equal(setup.elements.p.type,'text');
  setup.elements.show.onclick(); assert.equal(setup.elements.p.type,'password');
  setup.elements.p.value = 'mock-password'; setup.elements.c.value = 'different';
  await setup.form.onsubmit({preventDefault(){}});
  assert.equal(setup.writes.length, 0); assert.match(setup.elements.e.textContent, /do not match/);
  setup.elements.c.value = 'mock-password';
  await setup.form.onsubmit({preventDefault(){}});
  assert.equal(setup.writes[0].url, '/api/auth/setup'); assert.equal(setup.writes[0].body.confirm_password, 'mock-password'); assert.equal(setup.redirects[0], '/');
  const remote = await page(false, true);
  assert.equal(remote.elements.b.disabled, true); assert.equal(remote.elements.p.disabled, true); assert.match(remote.elements.intro.textContent, /home network/);
  const login = await page(true);
  assert.equal(login.elements.c.style.display, 'none'); login.elements.p.value = 'mock-password';
  await login.form.onsubmit({preventDefault(){}}); assert.equal(login.writes[0].url, '/api/auth/login'); assert.equal(login.redirects[0], '/');
  login.context.fetch = async () => ({ok:false,json:async()=>({error:'Password is incorrect.'})});
  await login.form.onsubmit({preventDefault(){}}); assert.equal(login.elements.e.textContent, 'Password is incorrect.');
  login.context.fetch = async () => { throw new Error('Network disconnected'); };
  await login.form.onsubmit({preventDefault(){}});
  assert.equal(login.elements.e.textContent, 'Network disconnected'); assert.equal(login.elements.b.disabled, false);
  login.context.fetch = async () => ({ok:false,status:403,json:async()=>{throw new Error('not JSON')}});
  await login.form.onsubmit({preventDefault(){}}); assert.match(login.elements.e.textContent, /unexpected response/);
  login.context.fetch = async () => { const err=new Error();err.name='AbortError';throw err; };
  await login.form.onsubmit({preventDefault(){}}); assert.match(login.elements.e.textContent, /20 seconds/);
  let complete;
  login.context.fetch = () => new Promise(resolve=>{complete=resolve});
  const pending=login.form.onsubmit({preventDefault(){}});
  assert.equal(login.elements.b.disabled,true); assert.equal(login.elements.b.textContent,'Signing in…');
  assert.match(login.elements.e.textContent,/Waiting/);
  complete({ok:true,json:async()=>({success:true})}); await pending;
  assert.match(login.elements.e.textContent,/Signed in/);
  console.log('PASS real login page: first-password confirmation, remote enrollment blocked, login, error display and redirect');
})().catch(error=>{console.error(error);process.exitCode=1;});
