/*
 * tween.js - minimal animation: "over N ms, call onUpdate(t) with t
 * going 0 -> 1 along an easing curve".
 *
 * Every camera fly-through and layer peel in the visualizer is one of
 * these. The engine's render loop calls tickAnimations() once per frame.
 */

export const ease = {
  linear: (t) => t,
  outCubic: (t) => 1 - Math.pow(1 - t, 3),
  inOutCubic: (t) => (t < 0.5 ? 4 * t * t * t : 1 - Math.pow(-2 * t + 2, 3) / 2),
  outBack: (t) => {
    const c1 = 1.4;
    const c3 = c1 + 1;
    return 1 + c3 * Math.pow(t - 1, 3) + c1 * Math.pow(t - 1, 2);
  },
};

const running = new Set();

/* Returns a handle with cancel(). onUpdate receives the eased t. */
export function animate(durationMs, onUpdate, { easing = ease.inOutCubic, onComplete } = {}) {
  const anim = {
    start: performance.now(),
    durationMs: Math.max(1, durationMs),
    onUpdate,
    easing,
    onComplete,
    cancel() {
      running.delete(anim);
    },
  };
  running.add(anim);
  onUpdate(easing(0));
  return anim;
}

export function tickAnimations(now) {
  for (const a of [...running]) {
    const raw = Math.min(1, (now - a.start) / a.durationMs);
    a.onUpdate(a.easing(raw));
    if (raw >= 1) {
      running.delete(a);
      if (a.onComplete) a.onComplete();
    }
  }
}

export const lerp = (a, b, t) => a + (b - a) * t;
