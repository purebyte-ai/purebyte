# VS Code tasks

Scan the file you are editing, your staged changes or the whole workspace without leaving the editor.

## Set up

1. Install the CLI and the model ([docs/install.md](../../docs/install.md)): `purebyte models pull secrets-code`.
2. Copy [`tasks.json`](tasks.json) to `.vscode/tasks.json` in your project (or merge its `tasks` into yours).
3. Run a task with **Terminal > Run Task...** and pick one of the `PureByte:` tasks.

| Task | What it does |
|---|---|
| PureByte: scan the current file | Scans the file in the active editor |
| PureByte: scan the staged changes | Scans what you are about to commit; findings on added lines only |
| PureByte: scan the workspace (SARIF report) | Writes `purebyte.sarif` at the root of the workspace |
| PureByte: start the local API | Runs `purebyte serve` on `127.0.0.1:8421` until you stop the task |

## Browse the findings

Open `purebyte.sarif` with the [SARIF Viewer](https://marketplace.visualstudio.com/items?itemName=MS-SarifVSCode.sarif-viewer)
extension to browse the findings with links to the exact lines. Add `purebyte.sarif` to your `.gitignore`: the report
is masked, but it has no place in the repository.

## Keyboard shortcut

To scan the current file with a key, add to your `keybindings.json`:

```json
{
  "key": "ctrl+alt+p",
  "command": "workbench.action.tasks.runTask",
  "args": "PureByte: scan the current file"
}
```
