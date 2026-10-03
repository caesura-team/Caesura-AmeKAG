/** Complete one warmup each, then serial A/B samples; preserve upper medians. */
export async function dualMedianMs(fnA, fnB, reps, nowNs = () => process.hrtime.bigint()) {
  await fnA(); await fnB() // warmup
  const a = [], b = []
  for (let i = 0; i < reps; i++) {
    let t0 = nowNs(); await fnA(); a.push(Number(nowNs() - t0) / 1e6)
    t0 = nowNs(); await fnB(); b.push(Number(nowNs() - t0) / 1e6)
  }
  a.sort((x, y) => x - y); b.sort((x, y) => x - y)
  return { srcMs: a[Math.floor(a.length / 2)], bndMs: b[Math.floor(b.length / 2)] }
}
