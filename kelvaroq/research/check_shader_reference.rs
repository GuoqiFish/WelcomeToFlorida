//! Standalone consumer of the pinned reference APIs. Not part of the product.

mod shader_reference_draw;

use std::io::{self, Read};
use victory_shader::{Container, DxilProgram, FourCC, HwStage, emit, gfx10, native_programs};
use windows::Win32::Graphics::{
    Direct3D::D3D_FEATURE_LEVEL_12_0,
    Direct3D12::D3D12CreateDevice,
    Dxgi::{
        CreateDXGIFactory2, DXGI_ADAPTER_FLAG_SOFTWARE, DXGI_CREATE_FACTORY_FLAGS,
        DXGI_ERROR_NOT_FOUND, IDXGIFactory4,
    },
};

fn record(rva: u32, step: &str, ok: bool, detail: impl std::fmt::Display) {
    let detail = detail.to_string().replace(['\t', '\r', '\n'], " ");
    println!("{rva:#x}\t{step}\t{ok}\t{detail}");
}

fn pipelines(shaders: &[(u32, emit::HostShader)], draw: bool) -> windows::core::Result<()> {
    // All hardware adapters get the same VS/PS candidate pairings and state.
    let factory: IDXGIFactory4 = unsafe { CreateDXGIFactory2(DXGI_CREATE_FACTORY_FLAGS(0))? };
    for index in 0.. {
        let adapter = match unsafe { factory.EnumAdapters1(index) } {
            Ok(adapter) => adapter,
            Err(e) if e.code() == DXGI_ERROR_NOT_FOUND => break,
            Err(e) => return Err(e),
        };
        let desc = unsafe { adapter.GetDesc1()? };
        if desc.Flags & DXGI_ADAPTER_FLAG_SOFTWARE.0 as u32 != 0 {
            continue;
        }
        let end = desc
            .Description
            .iter()
            .position(|&x| x == 0)
            .unwrap_or(desc.Description.len());
        let name = String::from_utf16_lossy(&desc.Description[..end]);
        let mut device = None;
        if let Err(error) =
            unsafe { D3D12CreateDevice(&adapter, D3D_FEATURE_LEVEL_12_0, &mut device) }
        {
            record(0, "adapter", false, format!("{name}: {error}"));
            continue;
        }
        let host = emit::host::Host {
            device: device.expect("successful device creation"),
        };
        record(0, "adapter", true, &name);
        for (vrva, vs) in shaders.iter().filter(|(_, p)| p.stage == HwStage::Vs) {
            for (prva, ps) in shaders.iter().filter(|(_, p)| p.stage == HwStage::Ps) {
                let result = host.pipeline(vs, ps);
                let pair = format!("{name}, VS {vrva:#x}");
                match result {
                    Ok((root, pso)) => {
                        record(*prva, "pipeline", true, &pair);
                        if draw
                            && (*vrva, *prva)
                                == (shader_reference_draw::VERTEX, shader_reference_draw::PIXEL)
                        {
                            let binding = |p: &emit::HostShader| {
                                p.user_data.is_empty()
                                    && p.root_cbvs.is_empty()
                                    && p.root_uavs.is_empty()
                                    && p.root_indexed_views.is_empty()
                                    && p.samplers.is_empty()
                            };
                            if !binding(vs)
                                || !binding(ps)
                                || vs.root_srvs != [0]
                                || !ps.root_srvs.is_empty()
                            {
                                record(
                                    *prva,
                                    "pixels",
                                    false,
                                    format!("{pair}: unexpected binding layout"),
                                );
                            } else {
                                let result =
                                    shader_reference_draw::pixels(&host.device, &root, &pso);
                                match result {
                                    Ok(()) => record(
                                        *prva,
                                        "pixels",
                                        true,
                                        format!("{pair}: 4/4 RGBA32F pixels"),
                                    ),
                                    Err(error) => {
                                        record(*prva, "pixels", false, format!("{pair}: {error}"))
                                    }
                                }
                            }
                        }
                    }
                    Err(error) => record(*prva, "pipeline", false, format!("{pair}: {error}")),
                }
            }
        }
    }
    Ok(())
}

fn main() -> Result<(), Box<dyn std::error::Error>> {
    let mut stream = Vec::new();
    io::stdin().read_to_end(&mut stream)?;
    let mut input = stream.as_slice();
    let mut shaders = Vec::new();
    while !input.is_empty() {
        let mut header = [0; 8];
        input.read_exact(&mut header)?;
        let rva = u32::from_le_bytes(header[..4].try_into()?);
        let size = u32::from_le_bytes(header[4..].try_into()?) as usize;
        let (data, rest) = input
            .split_at_checked(size)
            .ok_or("truncated shader input")?;
        input = rest;
        let container = match Container::parse(data) {
            Ok(c) => c,
            Err(error) => {
                record(rva, "container", false, error);
                continue;
            }
        };
        record(rva, "container", true, container.parts.len());
        let Some(dxil) = container.part(FourCC::DXIL) else {
            record(rva, "dxil", false, "missing part");
            continue;
        };
        match DxilProgram::parse(dxil.data) {
            Ok(p) => record(rva, "dxil", true, format!("{:?}", p.kind)),
            Err(error) => record(rva, "dxil", false, error),
        }
        match native_programs(dxil.data) {
            Ok(programs) => {
                record(rva, "native", true, programs.len());
                for p in programs {
                    let stage = format!("{:?}", p.stage);
                    record(rva, "stage", true, stage);
                    match gfx10::decode(&p.code) {
                        Ok(instructions) => record(rva, "decoded", true, instructions.len()),
                        Err(error) => record(rva, "decoded", false, error),
                    }
                }
            }
            Err(error) => record(rva, "native", false, error),
        }
        match emit::translate(data) {
            Ok(shader) => {
                record(rva, "translation", true, shader.dxil.len());
                record(
                    rva,
                    "bindings",
                    true,
                    format!(
                        "root_srvs={:?}, root_cbvs={:?}, root_uavs={:?}, user_dwords={}, indexed_views={}, samplers={}",
                        shader.root_srvs,
                        shader.root_cbvs,
                        shader.root_uavs,
                        shader.user_data_dwords(),
                        shader.root_indexed_views.len(),
                        shader.samplers.len()
                    ),
                );
                shaders.push((rva, shader));
            }
            Err(error) => record(rva, "translation", false, error),
        }
    }
    if let Some(mode) = std::env::args().nth(1) {
        if mode != "--pipelines" && mode != "--draw" {
            return Err("unknown probe mode".into());
        }
        pipelines(&shaders, mode == "--draw")?;
    }
    Ok(())
}
