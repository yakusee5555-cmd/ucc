/* UCC Playground frontend — Monaco editor, one-shot + interactive runs,
 * diagnostics markers, templates, compiler-internals drawer. */
(function () {
  'use strict';

  // ---------------------------------------------------------------- templates
  var TEMPLATES = [
    {
      name: 'Swap Two Numbers (swap(a, b))',
      code: '#include <stdio.h>\n\nint main() {\n    int a = 5, b = 10;\n    printf("Before: a = %d, b = %d\\n", a, b);\n    swap(a, b);\n    printf("After:  a = %d, b = %d\\n", a, b);\n    return 0;\n}\n'
    },
    {
      name: 'Multi-Variable Declarations & Pointers',
      code: '#include <stdio.h>\n\nint main() {\n    int x = 10, y = 20, *p, arr[5];\n    int i, sum = 0;\n    p = &x;\n    printf("x = %d, *p = %d\\n", x, *p);\n    *p = 42;\n    printf("after *p = 42: x = %d\\n", x);\n    for (i = 0; i < 5; i = i + 1) {\n        arr[i] = (i + 1) * 10;\n    }\n    for (i = 0; i < 5; i = i + 1) {\n        sum = sum + arr[i];\n    }\n    printf("sum of arr = %d\\n", sum);\n    return 0;\n}\n'
    },
    {
      name: 'Interactive scanf & printf Calculator',
      code: '#include <stdio.h>\n\nint main() {\n    int a, b, choice;\n    printf("Enter two integers: ");\n    scanf("%d %d", &a, &b);\n    printf("1=add 2=sub 3=mul 4=div, choose: ");\n    scanf("%d", &choice);\n    if (choice == 1) {\n        printf("%d + %d = %d\\n", a, b, a + b);\n    } else if (choice == 2) {\n        printf("%d - %d = %d\\n", a, b, a - b);\n    } else if (choice == 3) {\n        printf("%d * %d = %d\\n", a, b, a * b);\n    } else {\n        printf("%d / %d = %d\\n", a, b, a / b);\n    }\n    return 0;\n}\n'
    },
    {
      name: 'Sorting an Array with swap(arr[i], arr[j])',
      code: '#include <stdio.h>\n\nint main() {\n    int arr[8] = {42, 7, 19, 3, 88, 15, 64, 29};\n    int i, j, n = 8;\n    for (i = 0; i < n - 1; i = i + 1) {\n        for (j = 0; j < n - 1 - i; j = j + 1) {\n            if (arr[j] > arr[j + 1]) {\n                swap(arr[j], arr[j + 1]);\n            }\n        }\n    }\n    printf("Sorted: ");\n    for (i = 0; i < n; i = i + 1) {\n        printf("%d ", arr[i]);\n    }\n    printf("\\n");\n    return 0;\n}\n'
    },
    {
      name: 'Recursive Fibonacci',
      code: '#include <stdio.h>\n\nint fib(int n) {\n    if (n <= 1) {\n        return n;\n    }\n    return fib(n - 1) + fib(n - 2);\n}\n\nint main() {\n    int n, i;\n    printf("How many terms? ");\n    scanf("%d", &n);\n    for (i = 0; i < n; i = i + 1) {\n        printf("%d ", fib(i));\n    }\n    printf("\\n");\n    return 0;\n}\n'
    }
  ];

  // ------------------------------------------------------------------- state
  var editor = null;
  var monacoReady = false;
  var ws = null;
  var interactiveRunning = false;
  var httpRunning = false;
  var internalsOpen = false;
  var internalsData = { tokens: '', ast: '', bytecode: '' };
  var activeITab = 'tokens';

  // --------------------------------------------------------------------- dom
  function $(id) { return document.getElementById(id); }
  var runBtn = $('runBtn'), stopBtn = $('stopBtn'), themeBtn = $('themeBtn');
  var interactiveToggle = $('interactiveToggle'), internalsBtn = $('internalsBtn');
  var outputPane = $('outputPane'), errorPane = $('errorPane');
  var stdinText = $('stdinText'), stdinBox = $('stdinBox'), sendBtn = $('sendBtn');
  var badgeExit = $('badgeExit'), badgeTime = $('badgeTime'), badgeMem = $('badgeMem');
  var connState = $('connState'), errCount = $('errCount');
  var internalsPanel = $('internals'), internalsPane = $('internalsPane');

  // ------------------------------------------------------------------- theme
  function applyTheme(t) {
    document.documentElement.setAttribute('data-theme', t);
    try { localStorage.setItem('ucc-theme', t); } catch (e) {}
    themeBtn.textContent = t === 'dark' ? '🌙' : '☀️';
    if (monacoReady && window.monaco) {
      window.monaco.editor.setTheme(t === 'dark' ? 'vs-dark' : 'vs');
    }
  }
  var savedTheme = 'dark';
  try { savedTheme = localStorage.getItem('ucc-theme') || 'dark'; } catch (e) {}
  applyTheme(savedTheme);
  themeBtn.addEventListener('click', function () {
    applyTheme(document.documentElement.getAttribute('data-theme') === 'dark' ? 'light' : 'dark');
  });

  // -------------------------------------------------------------------- tabs
  function wireTabs(containerSel, paneFor) {
    var tabs = document.querySelectorAll(containerSel + ' .tab');
    tabs.forEach(function (tab) {
      tab.addEventListener('click', function () {
        tabs.forEach(function (t) { t.classList.remove('active'); });
        tab.classList.add('active');
        paneFor(tab);
      });
    });
  }
  wireTabs('.console-pane', function (tab) {
    var name = tab.getAttribute('data-tab');
    outputPane.classList.toggle('active', name === 'output');
    $('inputPane').classList.toggle('active', name === 'input');
    errorPane.classList.toggle('active', name === 'errors');
    if (name === 'errors') errCount.textContent = '';
  });
  wireTabs('.internals', function (tab) {
    activeITab = tab.getAttribute('data-itab');
    document.querySelectorAll('.internals .tab').forEach(function (t) {
      t.classList.toggle('active', t === tab);
    });
    internalsPane.textContent = internalsData[activeITab] || '(empty — run with this panel open)';
  });
  function showConsoleTab(name) {
    document.querySelectorAll('.console-pane .tab').forEach(function (t) {
      t.classList.toggle('active', t.getAttribute('data-tab') === name);
    });
    outputPane.classList.toggle('active', name === 'output');
    $('inputPane').classList.toggle('active', name === 'input');
    errorPane.classList.toggle('active', name === 'errors');
  }

  // ------------------------------------------------------------------ editor
  function defineUccLanguage() {
    var monaco = window.monaco;
    monaco.languages.register({ id: 'ucc-c' });
    monaco.languages.setMonarchTokensProvider('ucc-c', {
      defaultToken: '',
      tokenPostfix: '.c',
      keywords: [
        'auto', 'break', 'case', 'char', 'const', 'continue', 'default', 'do',
        'double', 'else', 'enum', 'extern', 'float', 'for', 'goto', 'if', 'int',
        'long', 'register', 'return', 'short', 'signed', 'sizeof', 'static',
        'struct', 'switch', 'typedef', 'union', 'unsigned', 'void', 'volatile',
        'while', '_Bool'
      ],
      intrinsics: ['swap'],
      builtins: ['printf', 'scanf'],
      tokenizer: {
        root: [
          [/[a-zA-Z_]\w*/, {
            cases: {
              '@keywords': 'keyword',
              '@intrinsics': 'keyword.intrinsic',
              '@builtins': 'type.identifier',
              '@default': 'identifier'
            }
          }],
          { include: '@whitespace' },
          [/[{}()\[\]]/, '@brackets'],
          [/[;,.]/, 'delimiter'],
          [/"([^"\\]|\\.)*$/, 'string.invalid'],
          [/"/, 'string', '@string'],
          [/'([^'\\]|\\.)*'/, 'string'],
          [/'([^'\\]|\\.)*$/, 'string.invalid'],
          [/0[xX][0-9a-fA-F]+[uUlL]*/, 'number.hex'],
          [/\d+\.\d*([eE][\-+]?\d+)?[fFlL]?/, 'number.float'],
          [/\d+[uUlL]*/, 'number'],
          [/\/\*/, 'comment', '@comment'],
          [/\/\/.*$/, 'comment'],
          [/[<>=!%&+\-*\^|?:]+/, 'operator']
        ],
        whitespace: [[/[ \t\r\n]+/, 'white']],
        comment: [
          [/[^\/*]+/, 'comment'],
          [/\*\//, 'comment', '@pop'],
          [/[\/*]/, 'comment']
        ],
        string: [
          [/[^\\"]+/, 'string'],
          [/\\./, 'string.escape'],
          [/"/, 'string', '@pop']
        ]
      }
    });
    monaco.languages.setLanguageConfiguration('ucc-c', {
      comments: { lineComment: '//', blockComment: ['/*', '*/'] },
      brackets: [['{', '}'], ['[', ']'], ['(', ')']],
      autoClosingPairs: [
        { open: '{', close: '}' }, { open: '[', close: ']' },
        { open: '(', close: ')' }, { open: '"', close: '"' }, { open: "'", close: "'" }
      ]
    });
    monaco.languages.registerCompletionItemProvider('ucc-c', {
      provideCompletionItems: function (model, position) {
        var word = model.getWordUntilPosition(position);
        var range = {
          startLineNumber: position.lineNumber,
          endLineNumber: position.lineNumber,
          startColumn: word.startColumn,
          endColumn: word.endColumn
        };
        var Kind = monaco.languages.CompletionItemKind;
        var items = [
          {
            label: 'swap(a, b)', kind: Kind.Snippet, insertTextRules: monaco.languages.CompletionItemInsertTextRule.InsertAsSnippet,
            insertText: 'swap(${1:a}, ${2:b});', range: range,
            documentation: 'Native intrinsic: atomically exchanges the values of two modifiable l-values.'
          },
          {
            label: 'printf', kind: Kind.Snippet, insertTextRules: monaco.languages.CompletionItemInsertTextRule.InsertAsSnippet,
            insertText: 'printf("${1:%d}\\n"${2:, ${3:x}});', range: range,
            documentation: 'Formatted output. Supports %d %i %u %f %lf %c %s %x %p.'
          },
          {
            label: 'scanf', kind: Kind.Snippet, insertTextRules: monaco.languages.CompletionItemInsertTextRule.InsertAsSnippet,
            insertText: 'scanf("${1:%d}", &${2:x});', range: range,
            documentation: 'Formatted input. Remember the & on each target variable.'
          },
          { label: 'swap', kind: Kind.Keyword, insertText: 'swap', range: range, documentation: 'Native swap intrinsic keyword.' },
          { label: 'int main', kind: Kind.Snippet, insertTextRules: monaco.languages.CompletionItemInsertTextRule.InsertAsSnippet, insertText: 'int main() {\n    ${1}\n    return 0;\n}', range: range },
          { label: 'for', kind: Kind.Snippet, insertTextRules: monaco.languages.CompletionItemInsertTextRule.InsertAsSnippet, insertText: 'for (${1:i} = 0; ${1:i} < ${2:n}; ${1:i} = ${1:i} + 1) {\n    ${3}\n}', range: range },
          { label: 'while', kind: Kind.Snippet, insertTextRules: monaco.languages.CompletionItemInsertTextRule.InsertAsSnippet, insertText: 'while (${1:cond}) {\n    ${2}\n}', range: range },
          { label: 'if', kind: Kind.Snippet, insertTextRules: monaco.languages.CompletionItemInsertTextRule.InsertAsSnippet, insertText: 'if (${1:cond}) {\n    ${2}\n}', range: range }
        ];
        return { suggestions: items };
      }
    });
  }

  function createFallbackEditor() {
    var el = $('editor');
    var ta = document.createElement('textarea');
    ta.id = 'fallbackEditor';
    ta.spellcheck = false;
    ta.value = TEMPLATES[0].code;
    el.parentNode.replaceChild(ta, el);
    editor = {
      getValue: function () { return ta.value; },
      setValue: function (v) { ta.value = v; },
      addCommand: function () {},
      revealLine: function () {}
    };
  }

  function initEditor() {
    if (typeof window.require === 'undefined') { createFallbackEditor(); return; }
    window.require.config({ paths: { vs: 'https://cdn.jsdelivr.net/npm/monaco-editor@0.52.2/min/vs' } });
    window.require(['vs/editor/editor.main'], function () {
      monacoReady = true;
      defineUccLanguage();
      editor = window.monaco.editor.create($('editor'), {
        value: TEMPLATES[0].code,
        language: 'ucc-c',
        theme: document.documentElement.getAttribute('data-theme') === 'dark' ? 'vs-dark' : 'vs',
        automaticLayout: true,
        fontSize: 14,
        minimap: { enabled: false },
        scrollBeyondLastLine: false,
        tabSize: 4
      });
      editor.addCommand(window.monaco.KeyMod.CtrlCmd | window.monaco.KeyCode.Enter, runCode);
    }, function () { createFallbackEditor(); });
  }

  // --------------------------------------------------------------- diagnostics
  function clearMarkers() {
    if (monacoReady && editor && editor.getModel) {
      window.monaco.editor.setModelMarkers(editor.getModel(), 'ucc', []);
    }
    errCount.textContent = '';
  }

  function setMarkers(diagnostics) {
    if (!monacoReady || !editor || !editor.getModel || !Array.isArray(diagnostics)) {
      if (Array.isArray(diagnostics) && diagnostics.length) errCount.textContent = diagnostics.length;
      return;
    }
    var Sev = window.monaco.MarkerSeverity;
    var markers = diagnostics.map(function (d) {
      var line = Math.max(1, d.line || 1);
      var col = Math.max(1, d.column || 1);
      var sev = String(d.severity || 'error').toLowerCase() === 'warning' ? Sev.Warning : Sev.Error;
      return {
        severity: sev,
        message: d.message || 'error',
        startLineNumber: line, startColumn: col,
        endLineNumber: line, endColumn: col + 1
      };
    });
    window.monaco.editor.setModelMarkers(editor.getModel(), 'ucc', markers);
    if (markers.length) {
      errCount.textContent = markers.length;
      try { editor.revealLine(markers[0].startLineNumber); } catch (e) {}
    }
  }

  // ------------------------------------------------------------------ output
  function appendOutput(t) { outputPane.textContent += t; outputPane.scrollTop = outputPane.scrollHeight; }
  function appendError(t) { errorPane.textContent += t; errorPane.scrollTop = errorPane.scrollHeight; }

  function fmtMem(b) {
    if (b === null || b === undefined) return 'mem: n/a';
    if (b < 1024) return 'mem: ' + b + ' B';
    if (b < 1048576) return 'mem: ' + (b / 1024).toFixed(1) + ' KB';
    return 'mem: ' + (b / 1048576).toFixed(2) + ' MB';
  }

  function setBadges(exitCode, timeMs, peakMemoryBytes) {
    badgeExit.textContent = 'exit: ' + (exitCode === null || exitCode === undefined ? '—' : exitCode);
    badgeExit.className = 'badge ' + (exitCode === 0 ? 'ok' : (exitCode === null || exitCode === undefined ? '' : 'bad'));
    badgeTime.textContent = 'time: ' + (timeMs === null || timeMs === undefined ? '—' : timeMs + ' ms');
    badgeMem.textContent = fmtMem(peakMemoryBytes);
  }

  function clearConsole() {
    outputPane.textContent = '';
    errorPane.textContent = '';
    errCount.textContent = '';
    setBadges(null, null, null);
  }

  function setBusy(busy) {
    runBtn.disabled = busy;
    connState.textContent = busy ? (interactiveRunning ? 'interactive…' : 'running…') : 'idle';
    connState.classList.toggle('live', !!interactiveRunning);
    stopBtn.disabled = !interactiveRunning;
  }

  function showResult(data) {
    outputPane.textContent = data.stdout || '';
    errorPane.textContent = data.stderr || '';
    setMarkers(data.diagnostics || []);
    setBadges(data.exitCode, data.timeMs, data.peakMemoryBytes);
    if ((data.stderr && data.stderr.trim()) || (data.diagnostics && data.diagnostics.length)) {
      showConsoleTab('errors');
    } else {
      showConsoleTab('output');
    }
    if (data.tokens !== undefined) {
      internalsData = { tokens: data.tokens || '', ast: data.ast || '', bytecode: data.bytecode || '' };
      internalsPane.textContent = internalsData[activeITab] || '(empty)';
    }
    outputPane.scrollTop = outputPane.scrollHeight;
    errorPane.scrollTop = errorPane.scrollHeight;
  }

  // ---------------------------------------------------------------- one-shot
  function runCode() {
    if (httpRunning || interactiveRunning) return;
    clearMarkers();
    if (interactiveToggle.checked) { startInteractive(); return; }
    httpRunning = true;
    setBusy(true);
    clearConsole();
    var payload = {
      source: editor.getValue(),
      stdin: stdinText.value,
      inspect: internalsOpen
    };
    fetch('/api/compile-run', {
      method: 'POST',
      headers: { 'Content-Type': 'application/json' },
      body: JSON.stringify(payload)
    })
      .then(function (resp) { return resp.json().then(function (d) { return { status: resp.status, body: d }; }); })
      .then(function (r) {
        if (r.status === 429) {
          showResult({ ok: false, stdout: '', stderr: 'server busy: too many concurrent executions — try again', exitCode: null, timeMs: 0, peakMemoryBytes: null, diagnostics: [] });
        } else {
          showResult(r.body);
        }
      })
      .catch(function (e) {
        showResult({ ok: false, stdout: '', stderr: 'request failed: ' + e.message, exitCode: null, timeMs: 0, peakMemoryBytes: null, diagnostics: [] });
      })
      .finally(function () { httpRunning = false; setBusy(false); });
  }

  // --------------------------------------------------------------- interactive
  function startInteractive() {
    stopInteractive(true);
    clearMarkers();
    clearConsole();
    interactiveRunning = true;
    setBusy(true);
    stdinBox.disabled = false;
    sendBtn.disabled = false;
    stdinBox.focus();

    var proto = location.protocol === 'https:' ? 'wss:' : 'ws:';
    ws = new WebSocket(proto + '//' + location.host + '/ws');
    var t0 = performance.now();

    ws.onopen = function () {
      ws.send(JSON.stringify({ type: 'start', source: editor.getValue() }));
    };
    ws.onmessage = function (ev) {
      var m;
      try { m = JSON.parse(ev.data); } catch (e) { return; }
      if (m.type === 'started') {
        appendOutput('[session started — type input below and press Send]\n');
      } else if (m.type === 'stdout') {
        appendOutput(m.data);
        showConsoleTab('output');
      } else if (m.type === 'stderr') {
        appendError(m.data);
      } else if (m.type === 'diagnostic') {
        setMarkers(m.diagnostics || []);
        showConsoleTab('errors');
      } else if (m.type === 'exit') {
        var ms = Math.round(performance.now() - t0);
        setBadges(m.code, m.timeMs !== undefined ? m.timeMs : ms, m.peakMemoryBytes);
        appendOutput('\n[exit code ' + m.code + ']\n');
        endInteractive();
      } else if (m.type === 'error') {
        appendError('[error] ' + m.message + '\n');
        showConsoleTab('errors');
      }
    };
    var onFail = function () {
      if (!interactiveRunning) return;
      appendError('[connection lost]\n');
      endInteractive();
    };
    ws.onclose = onFail;
    ws.onerror = onFail;
  }

  function endInteractive() {
    interactiveRunning = false;
    stdinBox.disabled = true;
    sendBtn.disabled = true;
    if (ws) { try { ws.close(); } catch (e) {} ws = null; }
    setBusy(false);
  }

  function stopInteractive(silent) {
    if (ws && ws.readyState === WebSocket.OPEN) {
      try { ws.send(JSON.stringify({ type: 'kill' })); } catch (e) {}
    }
    if (!silent) appendOutput('\n[stopped]\n');
    endInteractive();
  }

  function sendStdin() {
    if (!ws || ws.readyState !== WebSocket.OPEN || !interactiveRunning) return;
    var v = stdinBox.value;
    ws.send(JSON.stringify({ type: 'stdin', data: v + '\n' }));
    stdinBox.value = '';
    stdinBox.focus();
  }

  // ------------------------------------------------------------------ events
  runBtn.addEventListener('click', runCode);
  stopBtn.addEventListener('click', function () { stopInteractive(false); });
  sendBtn.addEventListener('click', sendStdin);
  stdinBox.addEventListener('keydown', function (e) {
    if (e.key === 'Enter') { e.preventDefault(); sendStdin(); }
  });
  $('clearBtn').addEventListener('click', function () { clearConsole(); clearMarkers(); });
  $('copyBtn').addEventListener('click', function () {
    var t = outputPane.textContent;
    if (navigator.clipboard && navigator.clipboard.writeText) {
      navigator.clipboard.writeText(t).catch(function () {});
    } else {
      var ta = document.createElement('textarea');
      ta.value = t;
      document.body.appendChild(ta);
      ta.select();
      try { document.execCommand('copy'); } catch (e) {}
      document.body.removeChild(ta);
    }
  });

  interactiveToggle.addEventListener('change', function () {
    var on = interactiveToggle.checked;
    if (!on && interactiveRunning) stopInteractive(true);
    if (!on) { stdinBox.disabled = true; sendBtn.disabled = true; }
  });

  internalsBtn.addEventListener('click', function () {
    internalsOpen = !internalsOpen;
    internalsPanel.classList.toggle('hidden', !internalsOpen);
    document.querySelector('.layout').classList.toggle('with-internals', internalsOpen);
    if (internalsOpen && window.monaco && editor && editor.layout) {
      setTimeout(function () { try { editor.layout(); } catch (e) {} }, 50);
    }
  });
  $('internalsClose').addEventListener('click', function () { internalsBtn.click(); });
  $('internalsLoad').addEventListener('click', function () {
    if (httpRunning || interactiveRunning) return;
    // force one inspect run regardless of one-shot/interactive choice
    var wasInteractive = interactiveToggle.checked;
    interactiveToggle.checked = false;
    var done = function () { interactiveToggle.checked = wasInteractive; };
    clearMarkers();
    httpRunning = true;
    setBusy(true);
    clearConsole();
    fetch('/api/compile-run', {
      method: 'POST',
      headers: { 'Content-Type': 'application/json' },
      body: JSON.stringify({ source: editor.getValue(), stdin: stdinText.value, inspect: true })
    })
      .then(function (r) { return r.json(); })
      .then(showResult)
      .catch(function (e) {
        showResult({ ok: false, stdout: '', stderr: 'request failed: ' + e.message, exitCode: null, timeMs: 0, peakMemoryBytes: null, diagnostics: [] });
      })
      .finally(function () { httpRunning = false; setBusy(false); done(); });
  });

  // ---------------------------------------------------------------- templates
  var templateSelect = $('templateSelect');
  TEMPLATES.forEach(function (t, i) {
    var opt = document.createElement('option');
    opt.value = String(i);
    opt.textContent = t.name;
    templateSelect.appendChild(opt);
  });
  templateSelect.addEventListener('change', function () {
    var t = TEMPLATES[Number(templateSelect.value)];
    if (t && editor) {
      editor.setValue(t.code);
      clearMarkers();
      clearConsole();
    }
  });

  // --------------------------------------------------------------------- init
  initEditor();
})();
