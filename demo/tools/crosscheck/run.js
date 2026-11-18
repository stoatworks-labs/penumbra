// The page's half of the crosscheck: fetch the cases and their frames from
// crosscheck.py's server, render each case through window.penumbraDemo.renderCase,
// draw the page's own person for comparison with the harness's cards, and post
// the pictures back, with the page's table of controls and any uniform the page
// set that a program lacks or a program has that nothing set. crosscheck.py
// compares; nothing is judged here.
const post = (path, body) => fetch(path, { method: 'POST', body });

async function main() {
  for (let i = 0; i < 400 && !window.penumbraDemo; i += 1) await new Promise((r) => setTimeout(r, 50));
  if (!window.penumbraDemo) throw new Error('window.penumbraDemo never appeared: the page did not start');
  const job = await (await fetch('/xc/job.json')).json();
  const frames = {};
  const frame = async (key) => {
    if (!frames[key]) frames[key] = new Uint8Array(await (await fetch(`/xc/frame/${key}`)).arrayBuffer());
    return frames[key];
  };
  await post('/xc/table', JSON.stringify(window.penumbraDemo.table));
  const missing = new Set();
  const unset = new Set();
  const errors = [];
  const refused = [];
  let passes = '';
  for (const c of job.cases) {
    const out = window.penumbraDemo.renderCase({
      settings: c.settings,
      width: c.width,
      height: c.height,
      dest: await frame(c.dest),
      src: await frame(c.src),
      srcWidth: c.srcWidth,
      srcHeight: c.srcHeight,
    });
    out.missing.forEach((name) => missing.add(name));
    out.unset.forEach((name) => unset.add(name));
    if (out.error) errors.push(`${c.name}: GL error ${out.error}`);
    if (!out.ok) refused.push(c.name);
    if (!passes && out.passes) passes = out.passes.map((p) => `${'xy'[p.axis]} ${p.stride}/${p.taps}`).join(', ');
    await post(`/xc/out/${c.name}`, out.pixels);
  }
  for (const p of job.people) {
    await post(`/xc/out/person-${p.id}`, window.penumbraDemo.person(p.id, p.width, p.height, 0));
  }
  await post('/xc/missing', JSON.stringify({ missing: [...missing], unset: [...unset], errors, refused, passes }));
  await post('/xc/done', 'ok');
}

main().catch((error) => post('/xc/error', String(error?.stack ?? error)));
