"use strict";

const vscode = require("vscode");
const fs = require("fs");
const path = require("path");
const cp = require("child_process");

const OUTPUT_NAME = "Yajir";
const LOADER_PATTERNS = [/^script_extension(?:_v\d+)?\.exe$/i];

let output;
let diagnostics;

function activeYajirDocument() {
  const editor = vscode.window.activeTextEditor;
  if (!editor || editor.document.languageId !== "yajir") {
    vscode.window.showWarningMessage("Open a .yaj file first.");
    return undefined;
  }
  if (editor.document.isUntitled) {
    vscode.window.showWarningMessage("Save the Yajir script before checking it.");
    return undefined;
  }
  return editor.document;
}

function workspaceRoot(document) {
  const folder = vscode.workspace.getWorkspaceFolder(document.uri);
  return folder ? folder.uri.fsPath : path.dirname(document.fileName);
}

function expandPath(value, document) {
  return value
    .replaceAll("${workspaceFolder}", workspaceRoot(document))
    .replaceAll("${fileDirname}", path.dirname(document.fileName));
}

function normalizePathSetting(value, document) {
  let expanded = expandPath(value.trim(), document);
  if (expanded.length >= 2) {
    const first = expanded[0];
    const last = expanded[expanded.length - 1];
    if ((first === "\"" && last === "\"") || (first === "'" && last === "'")) {
      expanded = expanded.slice(1, -1).trim();
    }
  }
  return path.isAbsolute(expanded)
    ? path.normalize(expanded)
    : path.resolve(workspaceRoot(document), expanded);
}

function configuredWorkingDirectory(document) {
  const config = vscode.workspace.getConfiguration("yajir", document.uri);
  const configured = config.get("workingDirectory", "${workspaceFolder}");
  return normalizePathSetting(configured, document);
}

function loaderCandidates(root) {
  const buildDir = path.join(root, "build");
  if (!fs.existsSync(buildDir)) {
    return [];
  }

  return fs.readdirSync(buildDir, { withFileTypes: true })
    .filter((entry) => entry.isFile() && LOADER_PATTERNS.some((pattern) => pattern.test(entry.name)))
    .map((entry) => {
      const fullPath = path.join(buildDir, entry.name);
      return { fullPath, name: entry.name, mtime: fs.statSync(fullPath).mtimeMs };
    })
    .sort((a, b) => {
      const rank = (name) => LOADER_PATTERNS.findIndex((pattern) => pattern.test(name));
      return rank(a.name) - rank(b.name) || b.mtime - a.mtime;
    });
}

function resolveLoader(document) {
  const config = vscode.workspace.getConfiguration("yajir", document.uri);
  const configured = config.get("loaderPath", "").trim();
  if (configured) {
    const fullPath = normalizePathSetting(configured, document);
    return fs.existsSync(fullPath) ? fullPath : undefined;
  }

  const candidates = loaderCandidates(workspaceRoot(document));
  return candidates.length ? candidates[0].fullPath : undefined;
}

async function requireLoader(document) {
  const loader = resolveLoader(document);
  if (loader) {
    return loader;
  }

  const choice = await vscode.window.showErrorMessage(
    "Yajir PC extension loader was not found. Set yajir.loaderPath to the loader executable.",
    "Open Settings"
  );
  if (choice === "Open Settings") {
    vscode.commands.executeCommand("workbench.action.openSettings", "yajir.loaderPath");
  }
  return undefined;
}

function appendProcessOutput(loader, args, cwd, stdout, stderr) {
  output.clear();
  output.appendLine(`> ${loader} ${args.map((arg) => JSON.stringify(arg)).join(" ")}`);
  output.appendLine(`cwd: ${cwd}`);
  if (stdout) {
    output.append(stdout.replace(/\r\n/g, "\n"));
    if (!stdout.endsWith("\n")) {
      output.appendLine("");
    }
  }
  if (stderr) {
    output.append(stderr.replace(/\r\n/g, "\n"));
    if (!stderr.endsWith("\n")) {
      output.appendLine("");
    }
  }
}

async function importedDocument(name, document) {
  const escaped = name.replace(/[\\{}[\]*?]/g, "\\$&");
  const matches = await vscode.workspace.findFiles(`**/${escaped}.yaj`, "**/build/**", 20);
  if (!matches.length) {
    return document;
  }

  const sameDirectory = matches.find((uri) => path.dirname(uri.fsPath) === path.dirname(document.fileName));
  return vscode.workspace.openTextDocument(sameDirectory || matches[0]);
}

async function publishLoadError(text, document) {
  const libraryMatch = text.match(/load error: in library '([^']+)' line (\d+):\s*([^\r\n]+)/);
  const mainMatch = text.match(/load error: line (\d+):\s*([^\r\n]+)/);
  let target = document;
  let line;
  let message;

  if (libraryMatch) {
    target = await importedDocument(libraryMatch[1], document);
    line = Number(libraryMatch[2]);
    message = libraryMatch[3];
  } else if (mainMatch) {
    line = Number(mainMatch[1]);
    message = mainMatch[2];
  } else {
    return false;
  }

  const lineIndex = Math.max(0, Math.min(target.lineCount - 1, line - 1));
  const range = target.lineAt(lineIndex).range;
  const diagnostic = new vscode.Diagnostic(range, message, vscode.DiagnosticSeverity.Error);
  diagnostic.source = "Yajir";
  diagnostics.set(target.uri, [diagnostic]);

  const editor = await vscode.window.showTextDocument(target, { preserveFocus: false, preview: true });
  editor.selection = new vscode.Selection(range.start, range.start);
  editor.revealRange(range, vscode.TextEditorRevealType.InCenterIfOutsideViewport);
  return true;
}

async function checkCurrentScript() {
  const document = activeYajirDocument();
  if (!document) {
    return;
  }
  if (document.isDirty && !(await document.save())) {
    return;
  }

  const loader = await requireLoader(document);
  if (!loader) {
    return;
  }

  diagnostics.clear();
  const cwd = configuredWorkingDirectory(document);
  const args = ["check", document.fileName];

  await vscode.window.withProgress(
    { location: vscode.ProgressLocation.Window, title: "Checking Yajir script..." },
    () => new Promise((resolve) => {
      cp.execFile(loader, args, { cwd, windowsHide: true, encoding: "utf8" }, async (error, stdout, stderr) => {
        appendProcessOutput(loader, args, cwd, stdout, stderr);
        const combined = `${stdout}\n${stderr}`;
        if (error) {
          const located = await publishLoadError(combined, document);
          output.show(true);
          if (!located) {
            vscode.window.showErrorMessage(`Yajir check failed (exit code ${error.code ?? "unknown"}).`);
          }
        } else {
          vscode.window.setStatusBarMessage("$(check) Yajir check passed", 3000);
          const reveal = vscode.workspace.getConfiguration("yajir", document.uri)
            .get("revealOutputOnSuccess", false);
          if (reveal) {
            output.show(true);
          }
        }
        resolve();
      });
    })
  );
}

async function runCurrentScript() {
  const document = activeYajirDocument();
  if (!document) {
    return;
  }
  if (document.isDirty && !(await document.save())) {
    return;
  }

  const loader = await requireLoader(document);
  if (!loader) {
    return;
  }

  const cwd = configuredWorkingDirectory(document);
  const seconds = vscode.workspace.getConfiguration("yajir", document.uri).get("runSeconds", -1);
  const args = ["run", document.fileName];
  if (seconds >= 0) {
    args.push(String(seconds));
  }

  const terminal = vscode.window.createTerminal({
    name: `Yajir: ${path.basename(document.fileName)}`,
    shellPath: loader,
    shellArgs: args,
    cwd
  });
  terminal.show();
}

function activate(context) {
  output = vscode.window.createOutputChannel(OUTPUT_NAME);
  diagnostics = vscode.languages.createDiagnosticCollection("yajir");

  context.subscriptions.push(
    output,
    diagnostics,
    vscode.commands.registerCommand("yajir.check", checkCurrentScript),
    vscode.commands.registerCommand("yajir.run", runCurrentScript),
    vscode.workspace.onDidChangeTextDocument((event) => diagnostics.delete(event.document.uri)),
    vscode.workspace.onDidCloseTextDocument((document) => diagnostics.delete(document.uri))
  );
}

function deactivate() {}

module.exports = { activate, deactivate };
