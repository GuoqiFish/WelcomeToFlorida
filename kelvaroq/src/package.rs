use anyhow::{Context, Result, bail, ensure};
use std::{
    fs,
    path::{Component, Path, PathBuf},
};

fn executable(path: &Path) -> bool {
    path.extension()
        .is_some_and(|ext| ext.eq_ignore_ascii_case("exe"))
}

pub fn image_path(selection: &Path) -> Result<PathBuf> {
    if !selection.is_dir() {
        return Ok(selection.to_owned());
    }
    let root = selection
        .canonicalize()
        .context("ouverture du dossier du jeu")?;
    let entries = fs::read_dir(&root)?
        .map(|entry| entry.map(|e| e.path()))
        .collect::<std::io::Result<Vec<_>>>()?;
    let manifest = entries.iter().find(|p| {
        p.file_name()
            .is_some_and(|n| n.eq_ignore_ascii_case("MicrosoftGame.config"))
    });
    let mut choices = if let Some(manifest) = manifest {
        let xml = fs::read_to_string(manifest).context("lecture de MicrosoftGame.config")?;
        manifest_executables(xml.trim_start_matches('\u{feff}'))?
            .into_iter()
            .map(|name| root.join(name))
            .collect()
    } else {
        entries
            .into_iter()
            .filter(|p| p.is_file() && executable(p))
            .collect::<Vec<_>>()
    };
    choices.sort();
    choices.dedup();
    let candidate = match choices.as_slice() {
        [one] => one,
        [] => bail!(
            "Aucun exécutable Series trouvé dans ce dossier. Choisissez directement le fichier .exe."
        ),
        _ => bail!(
            "Plusieurs exécutables sont possibles. Choisissez directement le fichier .exe voulu."
        ),
    };
    let resolved = candidate
        .canonicalize()
        .context("exécutable du dossier introuvable")?;
    ensure!(
        resolved.starts_with(&root) && resolved.is_file() && executable(&resolved),
        "L’exécutable doit se trouver dans le dossier du jeu."
    );
    Ok(resolved)
}

fn manifest_executables(xml: &str) -> Result<Vec<String>> {
    let doc = roxmltree::Document::parse(xml).context("MicrosoftGame.config invalide")?;
    let mut names = Vec::new();
    for list in doc
        .root_element()
        .children()
        .filter(|n| n.tag_name().name() == "ExecutableList")
    {
        for entry in list
            .children()
            .filter(|n| n.tag_name().name() == "Executable")
        {
            if entry
                .attribute("TargetDeviceFamily")
                .is_some_and(|family| !family.eq_ignore_ascii_case("Scarlett"))
            {
                continue;
            }
            let name = entry
                .attribute("Name")
                .context("nom d’exécutable absent du manifeste")?;
            let path = Path::new(name);
            ensure!(
                executable(path)
                    && path
                        .components()
                        .all(|c| matches!(c, Component::Normal(_) | Component::CurDir)),
                "Chemin d’exécutable invalide dans le manifeste."
            );
            names.push(name.to_owned());
        }
    }
    Ok(names)
}

#[cfg(test)]
mod tests {
    use super::*;

    #[test]
    fn resolves_series_entries_only_even_with_namespace() {
        let xml = r#"<Game xmlns="urn:test"><ExecutableList><Executable Name="pc.exe" TargetDeviceFamily="PC"/><Executable Name="sample.exe" TargetDeviceFamily="Scarlett"/></ExecutableList></Game>"#;
        assert_eq!(manifest_executables(xml).unwrap(), ["sample.exe"]);
    }

    #[test]
    fn rejects_parent_reference_from_manifest() {
        assert!(
            manifest_executables(
                r#"<Game><ExecutableList><Executable Name="../other.exe"/></ExecutableList></Game>"#
            )
            .is_err()
        );
    }
}
