use anyhow::{Context, Result, ensure};
use goblin::pe::PE;
use serde::Serialize;
use sha2::{Digest, Sha256};
use std::{fs, path::Path};

#[derive(Serialize)]
pub struct Section {
    name: String,
    rva: u32,
    virtual_size: u32,
    offset: u32,
    raw_size: u32,
    flags: u32,
}

#[derive(Serialize)]
pub struct Import {
    dll: String,
    name: String,
    iat: usize,
}

#[derive(Serialize)]
pub struct Image {
    pub sha256: String,
    base: u64,
    pub entry: u32,
    size: u32,
    headers: u32,
    pub sections: Vec<Section>,
    pub imports: Vec<Import>,
    pub callbacks: Vec<u64>,
    tls: Option<Tls>,
}
#[derive(Serialize)]
struct Tls {
    start: u64,
    end: u64,
    index: u64,
    zero_fill: u32,
}

pub fn inspect(path: &Path) -> Result<Image> {
    let bytes = fs::read(path).with_context(|| format!("reading {}", path.display()))?;
    parse(&bytes)
}

fn parse(bytes: &[u8]) -> Result<Image> {
    let pe = PE::parse(bytes).context("parsing PE headers")?;
    ensure!(
        pe.is_64 && pe.header.coff_header.machine == 0x8664 && !pe.is_lib,
        "expected an AMD64 executable"
    );
    let h = pe
        .header
        .optional_header
        .context("missing optional header")?
        .windows_fields;
    ensure!(
        h.size_of_headers > 0
            && h.size_of_headers as usize <= bytes.len()
            && h.size_of_headers <= h.size_of_image
            && pe
                .image_base
                .checked_add(u64::from(h.size_of_image))
                .is_some(),
        "invalid PE header size"
    );
    ensure!(
        pe.entry != 0 && pe.entry < h.size_of_image,
        "invalid entry RVA"
    );
    let mut sections = Vec::new();
    for s in &pe.sections {
        let end = u64::from(s.pointer_to_raw_data) + u64::from(s.size_of_raw_data);
        ensure!(
            s.size_of_raw_data == 0 || end <= bytes.len() as u64,
            "section extends past end of file"
        );
        let extent = u64::from(s.virtual_size.max(s.size_of_raw_data));
        ensure!(
            u64::from(s.virtual_address) + extent <= u64::from(h.size_of_image),
            "section extends past image"
        );
        sections.push(Section {
            name: s.name().unwrap_or("?").into(),
            rva: s.virtual_address,
            virtual_size: s.virtual_size,
            offset: s.pointer_to_raw_data,
            raw_size: s.size_of_raw_data,
            flags: s.characteristics,
        });
    }
    ensure!(
        sections.iter().any(|s| s.flags & 0x20000000 != 0
            && pe.entry >= s.rva
            && pe.entry - s.rva < s.virtual_size.max(s.raw_size)),
        "entry is outside executable sections"
    );
    let mut imports = Vec::with_capacity(pe.imports.len());
    for i in &pe.imports {
        ensure!(
            i.offset
                .checked_add(8)
                .is_some_and(|end| end <= h.size_of_image as usize),
            "IAT slot outside image"
        );
        imports.push(Import {
            dll: i.dll.to_ascii_lowercase(),
            name: i.name.to_string(),
            iat: i.offset,
        });
    }
    let callbacks = pe
        .tls_data
        .as_ref()
        .map(|t| t.callbacks.clone())
        .unwrap_or_default();
    let tls = pe.tls_data.as_ref().map(|t| {
        let d = &t.image_tls_directory;
        Tls {
            start: d.start_address_of_raw_data,
            end: d.end_address_of_raw_data,
            index: d.address_of_index,
            zero_fill: d.size_of_zero_fill,
        }
    });
    Ok(Image {
        sha256: format!("{:x}", Sha256::digest(bytes)),
        base: pe.image_base,
        entry: pe.entry,
        size: h.size_of_image,
        headers: h.size_of_headers,
        sections,
        imports,
        callbacks,
        tls,
    })
}

#[cfg(test)]
mod tests {
    #[test]
    fn truncated_header_fails() {
        assert!(super::parse(b"MZ").is_err());
    }
}
