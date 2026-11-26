(() => {
  if (globalThis.gizlet) return;
  const zh = globalThis.GIZLET_LANG === "zh-CN";
  const text = zh
    ? { shift: "大写", symbols: "符号", letters: "字母", space: "空格", del: "删除", go: "前往",
        hints: [["A", "输入"], ["B", "删除"], ["Y", "空格"], ["X", "符号"], ["L/R", "光标"], ["Start", "确定"], ["Select", "关闭"]],
        empty: "请输入网址或搜索词", invalid: "这个网址无法打开" }
    : { shift: "Shift", symbols: "#+=", letters: "abc", space: "Space", del: "Delete", go: "Go",
        hints: [["A", "Type"], ["B", "Delete"], ["Y", "Space"], ["X", "Symbols"], ["L/R", "Caret"], ["Start", "Go"], ["Select", "Close"]],
        empty: "Enter an address or search", invalid: "That address can't be opened" };
  const post = message => {
    try { webkit.messageHandlers.gizlet.postMessage(message); } catch (error) { }
  };
  const nonText = new Set(["button", "submit", "reset", "checkbox", "radio", "range", "color", "file", "image", "hidden"]);
  const editable = element => !!element && (element.isContentEditable || element.tagName === "TEXTAREA"
    || (element.tagName === "INPUT" && !nonText.has(element.type) && !element.readOnly && !element.disabled));
  let lastPointer = 0;

  if (window.top !== window) {
    addEventListener("mousedown", () => { lastPointer = Date.now(); }, true);
    addEventListener("focusin", event => {
      post("subfocus:" + (editable(event.target) ? (Date.now() - lastPointer < 600 ? "text-click" : "text") : "none"));
    }, true);
    addEventListener("focusout", () => post("subfocus:none"), true);
    return;
  }

  const start = location.protocol === "gizlet:";

  const css = `
  :host { all: initial; }
  * { box-sizing: border-box; }
  .layer { position: fixed; inset: 0; pointer-events: none; font: 15px/1.2 system-ui, "Noto Sans CJK SC", sans-serif; color: #faf7ff; }
  .progress { position: absolute; left: 0; top: 0; height: 3px; width: 0; background: #c9affc; box-shadow: 0 1px 3px rgba(0,0,0,.5); transition: width .2s ease-out, opacity .4s ease-out; }
  .ring { position: absolute; border: 3px solid #f9e28b; border-radius: 6px; box-shadow: 0 0 0 2px rgba(20,14,32,.75), 0 2px 8px rgba(0,0,0,.35); transition: transform .12s cubic-bezier(.2,.8,.2,1), width .12s cubic-bezier(.2,.8,.2,1), height .12s cubic-bezier(.2,.8,.2,1); will-change: transform; }
  .cursor { position: absolute; left: 0; top: 0; width: 22px; height: 22px; will-change: transform; transition: opacity .3s ease-out; }
  .toast { position: absolute; left: 50%; top: 14px; transform: translateX(-50%); padding: 7px 14px; border-radius: 16px; background: rgba(24,18,38,.92); box-shadow: 0 3px 10px rgba(0,0,0,.4); font-size: 15px; transition: opacity .25s ease-out; }
  .kb { position: absolute; left: 0; right: 0; bottom: 0; padding: 7px 8px 6px; background: #1d1729; box-shadow: 0 -4px 16px rgba(0,0,0,.45); border-top: 1px solid #3c3052; }
  .preview { height: 30px; margin: 0 0 6px; padding: 5px 10px; border-radius: 7px; background: #110d19; border: 1px solid #5a4870; font-size: 16px; white-space: pre; overflow: hidden; text-overflow: ellipsis; }
  .preview.placeholder { color: #a996c2; }
  .preview .caret { display: inline-block; width: 2px; height: 1.05em; margin: 0 -1px; vertical-align: -2px; background: #f9e28b; }
  .keys { display: grid; grid-template-columns: repeat(10, minmax(0, 1fr)); gap: 4px; }
  .key { height: 31px; display: flex; align-items: center; justify-content: center; border-radius: 6px; background: #342848; border: 1px solid #4f3f66; font-size: 16px; white-space: nowrap; overflow: hidden; transition: background .08s, transform .08s; }
  .key.quick { font-size: 14px; background: #2a2140; color: #ddd0f2; }
  .key.action { font-size: 14px; background: #2a2140; }
  .key.on { background: #5b4683; border-color: #c9affc; }
  .key.go { background: #c9affc; border-color: #c9affc; color: #1c1129; font-weight: 700; }
  .key.off { opacity: .4; }
  .key.selected { background: #f9e28b; border-color: #f9e28b; color: #1c1129; }
  .key.pressed { transform: scale(.92); }
  .hints { display: flex; flex-wrap: wrap; gap: 3px 11px; margin-top: 6px; font-size: 12px; color: #cdbde6; }
  .hints b { display: inline-block; min-width: 16px; padding: 0 4px; margin-right: 3px; border-radius: 4px; background: #3a2d52; color: #faf7ff; font-weight: 700; text-align: center; }
  [hidden] { display: none !important; }
  `;

  const host = document.createElement("gizlet-overlay");
  for (const [name, value] of [["all", "initial"], ["position", "fixed"], ["inset", "0"], ["z-index", "2147483647"], ["pointer-events", "none"], ["display", "block"]])
    host.style.setProperty(name, value, "important");
  const root = host.attachShadow({ mode: "closed" });
  try {
    const sheet = new CSSStyleSheet();
    sheet.replaceSync(css);
    root.adoptedStyleSheets = [sheet];
  } catch (error) {
    const style = document.createElement("style");
    style.textContent = css;
    root.append(style);
  }
  const layer = document.createElement("div");
  layer.className = "layer";
  layer.innerHTML = `
    <div class="progress"></div>
    <div class="ring" hidden></div>
    <svg class="cursor" hidden viewBox="0 0 22 22" aria-hidden="true"><path d="M3 2 L3 18 L7.3 14 L10.2 20.4 L13 19.1 L10.2 12.9 L16 12.9 Z" fill="#fff" stroke="#1c1129" stroke-width="1.6" stroke-linejoin="round"/></svg>
    <div class="toast" hidden></div>
    <section class="kb" hidden><div class="preview"></div><div class="keys"></div><div class="hints"></div></section>`;
  root.append(layer);
  const [progressBar, ring, cursor, toastBox, panel] = ["progress", "ring", "cursor", "toast", "kb"].map(name => layer.querySelector("." + name));
  const preview = panel.querySelector(".preview");
  const keysBox = panel.querySelector(".keys");
  panel.querySelector(".hints").innerHTML = text.hints.map(([button, label]) => `<span><b>${button}</b>${label}</span>`).join("");
  if (start) preview.hidden = true;

  const attach = () => {
    if (document.documentElement && !host.isConnected) document.documentElement.append(host);
  };
  attach();
  if (!host.isConnected) new MutationObserver((changes, observer) => {
    attach();
    if (host.isConnected) observer.disconnect();
  }).observe(document, { childList: true });

  const focusKind = element => {
    if (!element || element === document.body || element === document.documentElement) return "none";
    if (editable(element)) return "text";
    if (element.tagName === "IFRAME" || element.tagName === "FRAME") return "frame";
    if (element.tagName === "INPUT" && (element.type === "checkbox" || element.type === "radio")) return "toggle";
    return "other";
  };
  const target = () => {
    if (start) return document.getElementById("address");
    const element = document.activeElement;
    return editable(element) ? element : null;
  };

  const quickRow = [["https://", 2], ["www.", 2], [".com", 2], [".cn", 2], ["/", 1], [":", 1]];
  const digits = [..."1234567890"];
  const letterRows = [[..."qwertyuiop"], [..."asdfghjkl@"], [..."zxcvbnm.-_"]];
  const symbolRows = [[..."!#$%&*()+="], [..."?/\\;'\",[]~"], [..."<>{}|^`.-_"]];
  let shifted = false;
  let symbols = false;
  let rows = [];
  let row = 2;
  let column = 0;
  let open = false;
  let remote = false;

  function layout() {
    const layerRows = symbols ? symbolRows : letterRows;
    const letter = label => ({ label: shifted && !symbols ? label.toUpperCase() : label, insert: shifted && !symbols ? label.toUpperCase() : label });
    const actions = [
      { label: text.shift, run: toggleShift, span: 2, kind: "action", on: shifted && !symbols, off: symbols },
      { label: symbols ? text.letters : text.symbols, run: toggleSymbols, span: 2, kind: "action", on: symbols },
      { label: text.space, insert: " ", span: 2, kind: "action" },
      { label: text.del, run: erase, span: 2, kind: "action" },
      { label: text.go, run: go, span: 2, kind: "go" }
    ];
    rows = [
      quickRow.map(([label, span]) => ({ label, insert: label, span, kind: "quick" })),
      digits.map(label => ({ label, insert: label })),
      ...layerRows.map(keys => keys.map(letter)),
      actions
    ];
    for (const keys of rows) {
      let at = 0;
      for (const key of keys) {
        key.span ||= 1;
        key.from = at;
        at += key.span;
      }
    }
    column = Math.min(column, rows[row].length - 1);
    render();
  }

  function render() {
    keysBox.replaceChildren(...rows.flatMap((keys, r) => keys.map((key, c) => {
      const cell = document.createElement("div");
      cell.className = ["key", key.kind, key.on && "on", key.off && "off", r === row && c === column && "selected"].filter(Boolean).join(" ");
      cell.style.gridColumn = `span ${key.span}`;
      cell.textContent = key.label;
      key.cell = cell;
      return cell;
    })));
  }

  function select(nextRow, nextColumn) {
    rows[row][column].cell.classList.remove("selected");
    row = nextRow;
    column = nextColumn;
    rows[row][column].cell.classList.add("selected");
  }

  function move(direction) {
    if (direction === "left") return select(row, Math.max(0, column - 1));
    if (direction === "right") return select(row, Math.min(rows[row].length - 1, column + 1));
    const nextRow = Math.max(0, Math.min(rows.length - 1, row + (direction === "up" ? -1 : 1)));
    const key = rows[row][column];
    const middle = key.from + key.span / 2;
    const nextColumn = rows[nextRow].findIndex(candidate => middle >= candidate.from && middle < candidate.from + candidate.span);
    select(nextRow, nextColumn < 0 ? rows[nextRow].length - 1 : nextColumn);
  }

  function flash(key) {
    const cell = key && key.cell;
    if (!cell) return;
    cell.classList.add("pressed");
    setTimeout(() => cell.classList.remove("pressed"), 90);
  }

  function focusTarget() {
    const element = target();
    if (element && document.activeElement !== element) element.focus({ preventScroll: true });
    return element;
  }

  function insert(value) {
    if (remote) return post("type:" + value);
    const element = focusTarget();
    if (!element) return;
    if (!document.execCommand("insertText", false, value) && typeof element.setRangeText === "function") {
      element.setRangeText(value, element.selectionStart, element.selectionEnd, "end");
      element.dispatchEvent(new InputEvent("input", { bubbles: true, inputType: "insertText", data: value }));
    }
    update();
  }

  function erase() {
    if (remote) return post("erase");
    const element = focusTarget();
    if (!element) return;
    if (!document.execCommand("delete", false) && typeof element.setRangeText === "function") {
      const end = element.selectionEnd;
      const begin = element.selectionStart === end ? Math.max(0, end - 1) : element.selectionStart;
      element.setRangeText("", begin, end, "end");
      element.dispatchEvent(new InputEvent("input", { bubbles: true, inputType: "deleteContentBackward" }));
    }
    update();
  }

  function caret(step) {
    if (remote) return post("caret:" + step);
    const element = focusTarget();
    if (!element) return;
    try {
      const at = element.selectionStart;
      if (at === null || at === undefined) throw new Error("no selection");
      const next = Math.max(0, Math.min(element.value.length, (step < 0 ? Math.min(at, element.selectionEnd) : Math.max(at, element.selectionEnd)) + (element.selectionStart === element.selectionEnd ? step : 0)));
      element.setSelectionRange(next, next);
    } catch (error) {
      getSelection().modify("move", step < 0 ? "backward" : "forward", "character");
    }
    update();
  }

  function toggleShift() {
    if (symbols) return;
    shifted = !shifted;
    layout();
  }

  function toggleSymbols() {
    symbols = !symbols;
    layout();
  }

  function go() {
    const element = target();
    if (start) {
      const value = element ? element.value.trim() : "";
      if (value) post("open:" + value);
      else toast(text.empty);
      return;
    }
    close();
    post("submit");
  }

  function update() {
    preview.hidden = start || remote;
    if (start || remote || !open) return;
    const element = target();
    if (!element) return;
    let value = element.isContentEditable ? element.innerText.replace(/\s+/g, " ") : element.value;
    if (element.type === "password") value = "•".repeat(value.length);
    if (!value) {
      preview.className = "preview placeholder";
      preview.textContent = element.placeholder || element.getAttribute("aria-label") || "";
      return;
    }
    preview.className = "preview";
    let at = value.length;
    try {
      if (typeof element.selectionEnd === "number") at = element.selectionEnd;
    } catch (error) { }
    const before = value.slice(Math.max(0, at - 48), at);
    const after = value.slice(at, at + 24);
    const mark = document.createElement("span");
    mark.className = "caret";
    preview.replaceChildren(before, mark, after);
  }

  function reveal(element) {
    const rect = element.getBoundingClientRect();
    const limit = innerHeight - panel.offsetHeight - 10;
    if (rect.bottom > limit) scrollBy(0, rect.bottom - limit);
    else if (rect.top < 8) scrollBy(0, rect.top - 8);
  }

  function openKeyboard(inFrame) {
    remote = !!inFrame && !start;
    const element = remote ? null : target();
    if (!element && !remote) return false;
    if (element && !start) {
      try {
        if (element.selectionStart === 0 && element.selectionEnd === 0 && element.value)
          element.setSelectionRange(element.value.length, element.value.length);
      } catch (error) { }
    }
    open = true;
    panel.hidden = false;
    ring.hidden = true;
    layout();
    update();
    if (element && !start) reveal(element);
    post("keyboard:1");
    return true;
  }

  function close() {
    if (start) {
      post("cancel");
      return;
    }
    open = false;
    remote = false;
    panel.hidden = true;
    post("keyboard:0");
  }

  function button(name) {
    if (!open) return;
    const key = rows[row][column];
    switch (name) {
    case "up": case "down": case "left": case "right": move(name); break;
    case "a":
      flash(key);
      if (key.run) key.run();
      else insert(key.insert);
      break;
    case "b": erase(); break;
    case "y": insert(" "); break;
    case "x": toggleSymbols(); break;
    case "l2": toggleShift(); break;
    case "l1": caret(-1); break;
    case "r1": caret(1); break;
    case "start": go(); break;
    case "select": close(); break;
    }
  }

  let toastTimer = 0;
  function toast(message) {
    toastBox.textContent = message;
    toastBox.hidden = false;
    toastBox.style.opacity = "1";
    clearTimeout(toastTimer);
    toastTimer = setTimeout(() => { toastBox.style.opacity = "0"; }, 1300);
  }

  let progressTimer = 0;
  function progress(fraction) {
    attach();
    clearTimeout(progressTimer);
    progressBar.style.opacity = "1";
    progressBar.style.width = Math.round(Math.max(0.05, fraction) * 100) + "%";
    if (fraction >= 1) progressTimer = setTimeout(() => { progressBar.style.opacity = "0"; }, 250);
  }

  let lastKey = 0;
  let cursorTimer = 0;
  let ringFrame = 0;
  function placeRing() {
    ringFrame = 0;
    const element = document.activeElement;
    if (open || !element || element === document.body || element === document.documentElement || !cursor.hidden) {
      ring.hidden = true;
      return;
    }
    const rect = element.getBoundingClientRect();
    if (!rect.width && !rect.height) {
      ring.hidden = true;
      return;
    }
    ring.hidden = false;
    ring.style.width = rect.width + 8 + "px";
    ring.style.height = rect.height + 8 + "px";
    ring.style.transform = `translate(${rect.left - 4}px, ${rect.top - 4}px)`;
  }
  const scheduleRing = () => {
    if (!ringFrame && !ring.hidden) ringFrame = requestAnimationFrame(placeRing);
  };

  addEventListener("keydown", event => {
    if (!event.isTrusted) return;
    lastKey = Date.now();
    if (!cursor.hidden) cursor.hidden = true;
    if (start && event.key === "Enter" && event.target && event.target.id === "address") {
      event.preventDefault();
      go();
    }
  }, true);
  addEventListener("mousedown", () => {
    lastPointer = Date.now();
    ring.hidden = true;
  }, true);
  addEventListener("mousemove", event => {
    if (!event.isTrusted) return;
    cursor.hidden = false;
    cursor.style.opacity = "1";
    cursor.style.transform = `translate(${event.clientX - 3}px, ${event.clientY - 2}px)`;
    ring.hidden = true;
    clearTimeout(cursorTimer);
    cursorTimer = setTimeout(() => { cursor.style.opacity = "0"; cursorTimer = setTimeout(() => { cursor.hidden = true; }, 300); }, 4000);
  }, true);
  addEventListener("focusin", event => {
    const element = event.target;
    const isEditable = editable(element);
    post("focus:" + focusKind(element));
    if (!start && isEditable && Date.now() - lastPointer < 600) {
      openKeyboard();
      return;
    }
    if (Date.now() - lastKey < 1500 && cursor.hidden) {
      ring.hidden = false;
      placeRing();
    }
  }, true);
  addEventListener("focusout", () => {
    post("focus:none");
    setTimeout(() => {
      if (!editable(document.activeElement) && open && !start && !remote) close();
      placeRing();
    }, 0);
  }, true);
  addEventListener("input", update, true);
  const sync = () => {
    post("keyboard:" + (open ? 1 : 0));
    post("focus:" + focusKind(document.activeElement));
  };
  addEventListener("pageshow", event => { if (event.persisted) sync(); });
  addEventListener("scroll", scheduleRing, { capture: true, passive: true });
  addEventListener("resize", scheduleRing, { passive: true });

  const ready = () => {
    attach();
    if (!start) return;
    document.documentElement.dataset.lang = zh ? "zh" : "en";
    const element = target();
    if (!element) return;
    element.focus();
    element.select();
    openKeyboard();
  };
  if (document.readyState === "loading") document.addEventListener("DOMContentLoaded", ready, { once: true });
  else ready();

  Object.defineProperty(globalThis, "gizlet", {
    value: Object.freeze({
      button, openKeyboard, progress, toast, sync,
      say: key => toast(text[key] || key),
      type: value => insert(value),
      isOpen: () => open,
      keyCount: () => rows.flat().length
    })
  });
})();
