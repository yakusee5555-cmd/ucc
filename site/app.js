/* UCC Playground — static WebAssembly edition.
 * The C compiler runs 100% in the browser via Emscripten. No server. */
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
      code: '#include <stdio.h>\n\nint main() {\n    int a, b, choice;\n    printf("Enter two integers: ");\n    scanf("%d %d", &a, &b);\n    printf("1=add 2=sub 3=mul 4=div, choose: ");\n    scanf("%d", &choice);\n    if (choice == 1) {\n        printf("%d + %d = %d\\n", a, b, a + b);\n    } else if (choice == 2) {\n        printf("%d - %d = %d\\n", a, b, a - b);\n    } else if (choice == 3) {\n        printf("%d * %d = %d\\n", a, b, a * b);\n    } else {\n        printf("%d / %d = %d\\n", a, b, a / b);\n    }\n    return 0;\n}\n',
      stdin: '12 30\n1\n'
    },
    {
      name: 'Sorting an Array with swap(arr[i], arr[j])',
      code: '#include <stdio.h>\n\nint main() {\n    int arr[8] = {42, 7, 19, 3, 88, 15, 64, 29};\n    int i, j, n = 8;\n    for (i = 0; i < n - 1; i = i + 1) {\n        for (j = 0; j < n - 1 - i; j = j + 1) {\n            if (arr[j] > arr[j + 1]) {\n                swap(arr[j], arr[j + 1]);\n            }\n        }\n    }\n    printf("Sorted: ");\n    for (i = 0; i < n; i = i + 1) {\n        printf("%d ", arr[i]);\n    }\n    printf("\\n");\n    return 0;\n}\n'
    },
    {
      name: 'Recursive Fibonacci',
      code: '#include <stdio.h>\n\nint fib(int n) {\n    if (n <= 1) {\n        return n;\n    }\n    return fib(n - 1) + fib(n - 2);\n}\n\nint main() {\n    int n, i;\n    printf("How many terms? ");\n    scanf("%d", &n);\n    for (i = 0; i < n; i = i + 1) {\n        printf("%d ", fib(i));\n    }\n    printf("\\n");\n    return 0;\n}\n',
      stdin: '10\n'
    }
  ];

  // ------------------------------------------------------------------- state
  var editor = null;
  var monacoReady = false;
  var wasmReady = false;
  var UCC = null;
  var stdinBytes = new Uint8Array(0);
  var stdinPos = 0;
  var stdoutBytes = [];
  var stderrBytes = [];
  var internalsOpen = false;
  var activeITab = 'tokens';

  // --------------------------------------------------------------------- dom
  function $(id) { return document.getElementById(id); }
  var runBtn = $('runBtn'), themeBtn = $('themeBtn');
  var internalsBtn = $('internalsBtn');
  var outputPane = $('outputPane'), errorPane = $('errorPane');
  var stdinText = $('stdinText');
  var badgeExit = $('badgeExit'), badgeTime = $('badgeTime');
  var connState = $('connState'), errCount = $('errCount');
  var engineState = $('engineState');
  var internalsPanel = $('internals'), internalsPane = $('internalsPane');
  var templateSelect = $('templateSelect');

  // ------------------------------------------------------------- wasm engine
  function wasmRun(source, stdin, args) {
    stdoutBytes = []; stderrBytes = [];
    stdinBytes = new TextEncoder().encode(stdin || '');
    stdinPos = 0;
    UCC.FS.writeFile('/prog.c', source);
    var exitCode;
    try {
      exitCode = UCC.callMain(['/prog.c'].concat(args || []));
    } catch (e) {
      exitCode = 2;
      stderrBytes.push.apply(stderrBytes, Array.from(new TextEncoder().encode('wasm error: ' + (e && e.message || e))));
    }
    var dec = new TextDecoder();
    return {
      exitCode: exitCode,
      stdout: dec.decode(new Uint8Array(stdoutBytes)),
      stderr: dec.decode(new Uint8Array(stderrBytes))
    };
  }

  function initWasm() {
    connState.textContent = 'loading compiler…';
    createUCCModule({}).then(function (mod) {
      UCC = mod;
      // stdin (fd 0): feed from buffer, null = EOF
      UCC.FS.streams[0].tty.ops.get_char = function () {
        return stdinPos < stdinBytes.length ? stdinBytes[stdinPos++] : null;
      };
      // stdout/stderr (fd 1/2): exact byte capture
      UCC.FS.streams[1].tty.ops.put_char = function (tty, val) {
        if (val !== null && val !== undefined) stdoutBytes.push(val);
      };
      UCC.FS.streams[2].tty.ops.put_char = function (tty, val) {
        if (val !== null && val !== undefined) stderrBytes.push(val);
      };
      wasmReady = true;
      runBtn.disabled = false;
      connState.textContent = 'ready · in-browser';
      engineState.textContent = 'compiler ready — runs 100% in your browser';
    }).catch(function (e) {
      connState.textContent = 'failed to load';
      engineState.textContent = 'could not load the WebAssembly compiler: ' + (e && e.message || e);
    });
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
    var markers = diagnostics.map(function (d) {
      return {
        severity: d.severity === 'warning'
          ? window.monaco.MarkerSeverity.Warning
          : window.monaco.MarkerSeverity.Error,
        message: d.message,
        startLineNumber: Math.max(1, d.line || 1),
        startColumn: Math.max(1, d.column || 1),
        endLineNumber: Math.max(1, d.line || 1),
        endColumn: Math.max(1, (d.column || 1) + 1)
      };
    });
    window.monaco.editor.setModelMarkers(editor.getModel(), 'ucc', markers);
    errCount.textContent = diagnostics.length ? diagnostics.length : '';
    if (diagnostics.length && editor.revealLine) {
      editor.revealLine(Math.max(1, diagnostics[0].line || 1));
    }
  }

  function getDiagnostics(source) {
    var r = wasmRun(source, '', ['--json-diagnostics']);
    try {
      var arr = JSON.parse(r.stdout);
      return Array.isArray(arr) ? arr : [];
    } catch (e) {
      return [];
    }
  }

  // ------------------------------------------------------------------ result
  function showResult(res) {
    outputPane.textContent = res.stdout || '(no output)';
    var errText = res.stderr || '';
    errorPane.textContent = errText;
    badgeExit.textContent = 'exit: ' + (res.exitCode === null || res.exitCode === undefined ? '—' : res.exitCode);
    badgeTime.textContent = 'time: ' + res.timeMs + ' ms';
    if (errText.trim()) switchTab('errors');
    else switchTab('output');
  }

  function runCode() {
    if (!wasmReady || !editor) return;
    var source = editor.getValue();
    var stdin = stdinText.value;
    clearMarkers();
    outputPane.textContent = 'compiling…';
    errorPane.textContent = '';
    badgeExit.textContent = 'exit: —';
    // let the UI paint before the (fast but synchronous) WASM call
    setTimeout(function () {
      var t0 = performance.now();
      var diags = getDiagnostics(source);
      setMarkers(diags);
      var res;
      var hasError = diags.some(function (d) { return d.severity === 'error'; });
      if (hasError) {
        res = { exitCode: 1, stdout: '', stderr: '', timeMs: 0 };
      } else {
        var r = wasmRun(source, stdin, []);
        res = { exitCode: r.exitCode, stdout: r.stdout, stderr: r.stderr, timeMs: 0 };
      }
      res.timeMs = Math.max(1, Math.round(performance.now() - t0));
      showResult(res);
      if (internalsOpen) loadInternals(source);
    }, 30);
  }

  // --------------------------------------------------------------- internals
  function loadInternals(source) {
    var src = source !== undefined ? source : (editor ? editor.getValue() : '');
    var map = {
      tokens: ['--dump-tokens'],
      ast: ['--dump-ast'],
      bytecode: ['--dump-bytecode']
    };
    var r = wasmRun(src, '', map[activeITab]);
    internalsPane.textContent = r.stdout || r.stderr || '(empty)';
  }

  // -------------------------------------------------------------------- tabs
  function switchTab(name) {
    var tabs = document.querySelectorAll('.console-pane .tab[data-tab]');
    for (var i = 0; i < tabs.length; i++) {
      tabs[i].classList.toggle('active', tabs[i].getAttribute('data-tab') === name);
    }
    outputPane.classList.toggle('active', name === 'output');
    $('inputPane').classList.toggle('active', name === 'input');
    errorPane.classList.toggle('active', name === 'errors');
  }

  function switchITab(name) {
    activeITab = name;
    var tabs = internalsPanel.querySelectorAll('.tab[data-itab]');
    for (var i = 0; i < tabs.length; i++) {
      tabs[i].classList.toggle('active', tabs[i].getAttribute('data-itab') === name);
    }
    loadInternals();
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
          [/[<>=!%&+\-*^|?:]+/, 'operator']
        ],
        whitespace: [[/[ \t\r\n]+/, 'white']],
        comment: [
          [/[^\\/*]+/, 'comment'],
          [/\*\//, 'comment', '@pop'],
          [/[\\/*]/, 'comment']
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

  // ------------------------------------------------------------------- theme
  function initTheme() {
    themeBtn.addEventListener('click', function () {
      var html = document.documentElement;
      var next = html.getAttribute('data-theme') === 'dark' ? 'light' : 'dark';
      html.setAttribute('data-theme', next);
      themeBtn.textContent = next === 'dark' ? '🌙' : '☀️';
      if (monacoReady && editor && editor.updateOptions) {
        editor.updateOptions({ theme: next === 'dark' ? 'vs-dark' : 'vs' });
      }
    });
  }

  // ------------------------------------------------------------------- wiring
  function initTemplates() {
    TEMPLATES.forEach(function (t, i) {
      var opt = document.createElement('option');
      opt.value = i;
      opt.textContent = t.name;
      templateSelect.appendChild(opt);
    });
    templateSelect.addEventListener('change', function () {
      var t = TEMPLATES[+templateSelect.value];
      if (editor) editor.setValue(t.code);
      stdinText.value = t.stdin || '';
      clearMarkers();
      outputPane.textContent = '';
      errorPane.textContent = '';
    });
  }

  function initTabs() {
    var tabs = document.querySelectorAll('.console-pane .tab[data-tab]');
    for (var i = 0; i < tabs.length; i++) {
      tabs[i].addEventListener('click', (function (el) {
        return function () { switchTab(el.getAttribute('data-tab')); };
      })(tabs[i]));
    }
    var itabs = internalsPanel.querySelectorAll('.tab[data-itab]');
    for (var j = 0; j < itabs.length; j++) {
      itabs[j].addEventListener('click', (function (el) {
        return function () { switchITab(el.getAttribute('data-itab')); };
      })(itabs[j]));
    }
  }

  function init() {
    initTemplates();
    initTabs();
    initTheme();
    initEditor();
    initWasm();
    runBtn.addEventListener('click', runCode);
    internalsBtn.addEventListener('click', function () {
      internalsOpen = !internalsOpen;
      internalsPanel.classList.toggle('hidden', !internalsOpen);
      if (internalsOpen) loadInternals();
    });
    $('internalsClose').addEventListener('click', function () {
      internalsOpen = false;
      internalsPanel.classList.add('hidden');
    });
    $('internalsLoad').addEventListener('click', function () { loadInternals(); });
    $('copyBtn').addEventListener('click', function () {
      var t = outputPane.textContent || '';
      if (navigator.clipboard) navigator.clipboard.writeText(t).catch(function () {});
    });
    $('clearBtn').addEventListener('click', function () {
      outputPane.textContent = '';
      errorPane.textContent = '';
      clearMarkers();
    });
    document.addEventListener('keydown', function (e) {
      if ((e.ctrlKey || e.metaKey) && e.key === 'Enter') { e.preventDefault(); runCode(); }
    });
  }

  if (document.readyState === 'loading') {
    document.addEventListener('DOMContentLoaded', init);
  } else {
    init();
  }
})();
