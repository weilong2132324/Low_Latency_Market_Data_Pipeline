/*
 * Navigator.js - the stack of views behind the breadcrumb.
 *
 *   Overview  >  Machine B  >  Motherboard  >  CPU die  >  Cache line
 *   \______/     \_____________________________________________/
 *    a View       one MachineView contributing four crumbs
 *
 * push() drills down, back() (Esc) goes up one level - first inside the
 * current view (e.g. CPU die -> motherboard), then out of it.
 */
export class Navigator {
  constructor(app) {
    this.app = app;
    this.stack = [];
  }

  get current() {
    return this.stack[this.stack.length - 1] || null;
  }

  push(view) {
    if (this.current) {
      this.current.active = false;
      this.current.exit();
    }
    this.stack.push(view);
    this._activate(view);
  }

  back() {
    if (!this.current) return;
    if (this.current.back()) {
      this.refresh();
      return;
    }
    if (this.stack.length > 1) this.popTo(this.stack.length - 2);
  }

  popTo(index) {
    while (this.stack.length - 1 > index) {
      const v = this.stack.pop();
      v.active = false;
      v.exit();
    }
    this._activate(this.current);
  }

  _activate(view) {
    view.active = true;
    this.app.engine.setView(view);
    view.enter();
    this.refresh();
  }

  /* Rebuild the breadcrumb; views call this when their internal level changes. */
  refresh() {
    const items = [];
    this.stack.forEach((view, idx) => {
      for (const c of view.crumbs()) {
        items.push({
          label: c.label,
          onClick: () => {
            if (idx !== this.stack.length - 1) this.popTo(idx);
            if (c.onClick) c.onClick();
            this.refresh();
          },
        });
      }
    });
    this.app.hud.setCrumbs(items);
  }
}
