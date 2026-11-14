// The heff language server for `.hf` files: `<heff> lsp --stdio`, the
// heff of the `lsp.heff.binary` setting, else the `heff` on the PATH,
// else the heff home's (`HEFF_HOME/bin/heff`, else `~/.heff/bin/heff`).

use zed_extension_api::{self as zed, settings::LspSettings, LanguageServerId, Result};

struct Heff;

fn home_heff(env: &[(String, String)]) -> Option<String> {
    let get = |key: &str| env.iter().find(|(k, _)| k == key).map(|(_, v)| v.clone());
    let home = get("HEFF_HOME").or_else(|| get("HOME").map(|h| format!("{h}/.heff")))?;
    Some(format!("{home}/bin/heff"))
}

impl zed::Extension for Heff {
    fn new() -> Self {
        Heff
    }

    fn language_server_command(
        &mut self,
        _id: &LanguageServerId,
        worktree: &zed::Worktree,
    ) -> Result<zed::Command> {
        let env = worktree.shell_env();
        let binary = LspSettings::for_worktree("heff", worktree)
            .ok()
            .and_then(|s| s.binary);
        let args = binary
            .as_ref()
            .and_then(|b| b.arguments.clone())
            .unwrap_or_else(|| vec!["lsp".into(), "--stdio".into()]);
        let command = binary
            .and_then(|b| b.path)
            .or_else(|| worktree.which("heff"))
            .or_else(|| home_heff(&env))
            .ok_or("heff: no heff on the PATH; put a release's bin/heff on it, or set lsp.heff.binary.path")?;
        Ok(zed::Command { command, args, env })
    }
}

zed::register_extension!(Heff);
