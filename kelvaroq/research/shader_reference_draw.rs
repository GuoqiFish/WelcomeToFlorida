//! Offscreen readback for a specific measured vertex/pixel pair. All vertices
//! and colors originate in this probe, and no title resource is bound.

use std::{
    error::Error,
    mem::ManuallyDrop,
    ptr,
    time::{Duration, Instant},
};
use windows::{
    Win32::{
        Foundation::RECT,
        Graphics::{Direct3D::D3D_PRIMITIVE_TOPOLOGY_TRIANGLELIST, Direct3D12::*, Dxgi::Common::*},
    },
    core::Interface,
};

const COLOR: [f32; 4] = [0.25, 0.5, 0.75, 1.0];
pub const VERTEX: u32 = 0x688e13c;
pub const PIXEL: u32 = 0x688f346;

fn vertices() -> [u32; 32] {
    let mut words = [0; 32];
    // Header type = low four bits, stride = (header >> 3) & 60 and
    // byte offset = (header >> 7) & 0x01ff_fffc in both attribute fetches.
    // Store the 3 positions at offset 32; place the 3 colors at offset 80.
    words[0] = (32 << 7) | (12 << 3) | 3;
    words[3] = (80 << 7) | (16 << 3) | 4;
    let positions: [[f32; 3]; 3] = [[-1., 0., 0.5], [3., 0., 0.5], [-1., 2., 0.5]];
    for (i, xyz) in positions.into_iter().enumerate() {
        words[8 + i * 3..11 + i * 3].copy_from_slice(&xyz.map(f32::to_bits));
        words[20 + i * 4..24 + i * 4].copy_from_slice(&COLOR.map(f32::to_bits));
    }
    words
}

fn resource(
    device: &ID3D12Device,
    heap: D3D12_HEAP_TYPE,
    state: D3D12_RESOURCE_STATES,
    desc: &D3D12_RESOURCE_DESC,
) -> Result<ID3D12Resource, Box<dyn Error>> {
    let props = D3D12_HEAP_PROPERTIES {
        Type: heap,
        CreationNodeMask: 1,
        VisibleNodeMask: 1,
        ..Default::default()
    };
    let mut object = None;
    unsafe {
        device.CreateCommittedResource(
            &props,
            D3D12_HEAP_FLAG_NONE,
            desc,
            state,
            None,
            &mut object,
        )?;
    }
    object.ok_or_else(|| "D3D12 returned no resource".into())
}

fn buffer(size: u64) -> D3D12_RESOURCE_DESC {
    D3D12_RESOURCE_DESC {
        Dimension: D3D12_RESOURCE_DIMENSION_BUFFER,
        Width: size,
        Height: 1,
        DepthOrArraySize: 1,
        MipLevels: 1,
        Layout: D3D12_TEXTURE_LAYOUT_ROW_MAJOR,
        SampleDesc: DXGI_SAMPLE_DESC {
            Count: 1,
            Quality: 0,
        },
        ..Default::default()
    }
}

pub fn pixels(
    device: &ID3D12Device,
    root: &ID3D12RootSignature,
    pso: &ID3D12PipelineState,
) -> Result<(), Box<dyn Error>> {
    let tex_desc = D3D12_RESOURCE_DESC {
        Dimension: D3D12_RESOURCE_DIMENSION_TEXTURE2D,
        Width: 2,
        Height: 2,
        DepthOrArraySize: 1,
        MipLevels: 1,
        Format: DXGI_FORMAT_R32G32B32A32_FLOAT,
        SampleDesc: DXGI_SAMPLE_DESC {
            Count: 1,
            Quality: 0,
        },
        Flags: D3D12_RESOURCE_FLAG_ALLOW_RENDER_TARGET,
        ..Default::default()
    };
    let target = resource(
        device,
        D3D12_HEAP_TYPE_DEFAULT,
        D3D12_RESOURCE_STATE_RENDER_TARGET,
        &tex_desc,
    )?;
    let upload = resource(
        device,
        D3D12_HEAP_TYPE_UPLOAD,
        D3D12_RESOURCE_STATE_GENERIC_READ,
        &buffer(128),
    )?;
    // All resources, heaps, command lists and descriptors remain alive through
    // completion of this list. No caller-provided memory enters the GPU buffer.
    unsafe {
        let mut footprint = D3D12_PLACED_SUBRESOURCE_FOOTPRINT::default();
        let mut size = 0;
        device.GetCopyableFootprints(
            &tex_desc,
            0,
            1,
            0,
            Some(&mut footprint),
            None,
            None,
            Some(&mut size),
        );
        if !(288..=512).contains(&size)
            || footprint.Footprint.RowPitch != 256
            || footprint.Offset != 0
        {
            return Err("unexpected 2x2 RGBA32F readback footprint".into());
        }
        let readback = resource(
            device,
            D3D12_HEAP_TYPE_READBACK,
            D3D12_RESOURCE_STATE_COPY_DEST,
            &buffer(size),
        )?;
        let mut mapped = ptr::null_mut();
        upload.Map(0, Some(&D3D12_RANGE::default()), Some(&mut mapped))?;
        let data = vertices();
        ptr::copy_nonoverlapping(data.as_ptr(), mapped.cast(), data.len());
        upload.Unmap(0, Some(&D3D12_RANGE { Begin: 0, End: 128 }));

        let heap = |kind, flags| {
            device.CreateDescriptorHeap::<ID3D12DescriptorHeap>(&D3D12_DESCRIPTOR_HEAP_DESC {
                Type: kind,
                NumDescriptors: 1,
                Flags: flags,
                ..Default::default()
            })
        };
        let rtvs = heap(
            D3D12_DESCRIPTOR_HEAP_TYPE_RTV,
            D3D12_DESCRIPTOR_HEAP_FLAG_NONE,
        )?;
        let views = heap(
            D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV,
            D3D12_DESCRIPTOR_HEAP_FLAG_SHADER_VISIBLE,
        )?;
        let samplers = heap(
            D3D12_DESCRIPTOR_HEAP_TYPE_SAMPLER,
            D3D12_DESCRIPTOR_HEAP_FLAG_SHADER_VISIBLE,
        )?;
        let rtv = rtvs.GetCPUDescriptorHandleForHeapStart();
        device.CreateRenderTargetView(&target, None, rtv);
        let queue: ID3D12CommandQueue =
            device.CreateCommandQueue(&D3D12_COMMAND_QUEUE_DESC::default())?;
        let alloc: ID3D12CommandAllocator =
            device.CreateCommandAllocator(D3D12_COMMAND_LIST_TYPE_DIRECT)?;
        let list: ID3D12GraphicsCommandList =
            device.CreateCommandList(0, D3D12_COMMAND_LIST_TYPE_DIRECT, &alloc, pso)?;
        let fence: ID3D12Fence = device.CreateFence(0, D3D12_FENCE_FLAG_NONE)?;
        list.SetDescriptorHeaps(&[Some(views.clone()), Some(samplers.clone())]);
        list.SetGraphicsRootSignature(root);
        // The reference root signature puts VS and PS user data at 0 and 1,
        // then its only root view, the VS buffer t0, at parameter index 2.
        list.SetGraphicsRoot32BitConstant(0, 0, 0);
        list.SetGraphicsRoot32BitConstant(1, 0, 0);
        list.SetGraphicsRootShaderResourceView(2, upload.GetGPUVirtualAddress());
        list.OMSetRenderTargets(1, Some(&rtv), false, None);
        list.ClearRenderTargetView(rtv, &[0.; 4], None);
        list.RSSetViewports(&[D3D12_VIEWPORT {
            Width: 2.,
            Height: 2.,
            MaxDepth: 1.,
            ..Default::default()
        }]);
        list.RSSetScissorRects(&[RECT {
            left: 0,
            top: 0,
            right: 2,
            bottom: 2,
        }]);
        list.IASetPrimitiveTopology(D3D_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
        list.DrawInstanced(3, 1, 0, 0);
        let mut barrier = D3D12_RESOURCE_BARRIER {
            Type: D3D12_RESOURCE_BARRIER_TYPE_TRANSITION,
            Anonymous: D3D12_RESOURCE_BARRIER_0 {
                Transition: ManuallyDrop::new(D3D12_RESOURCE_TRANSITION_BARRIER {
                    pResource: ManuallyDrop::new(Some(target.clone())),
                    Subresource: 0,
                    StateBefore: D3D12_RESOURCE_STATE_RENDER_TARGET,
                    StateAfter: D3D12_RESOURCE_STATE_COPY_SOURCE,
                }),
            },
            ..Default::default()
        };
        list.ResourceBarrier(std::slice::from_ref(&barrier));
        let transition = &mut *barrier.Anonymous.Transition;
        ManuallyDrop::drop(&mut transition.pResource);
        let mut dst = D3D12_TEXTURE_COPY_LOCATION {
            pResource: ManuallyDrop::new(Some(readback.clone())),
            Type: D3D12_TEXTURE_COPY_TYPE_PLACED_FOOTPRINT,
            Anonymous: D3D12_TEXTURE_COPY_LOCATION_0 {
                PlacedFootprint: footprint,
            },
        };
        let mut src = D3D12_TEXTURE_COPY_LOCATION {
            pResource: ManuallyDrop::new(Some(target.clone())),
            Type: D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX,
            Anonymous: D3D12_TEXTURE_COPY_LOCATION_0 {
                SubresourceIndex: 0,
            },
        };
        list.CopyTextureRegion(&dst, 0, 0, 0, &src, None);
        ManuallyDrop::drop(&mut dst.pResource);
        ManuallyDrop::drop(&mut src.pResource);
        list.Close()?;
        queue.ExecuteCommandLists(&[Some(list.cast()?)]);
        queue.Signal(&fence, 1)?;
        let start = Instant::now();
        loop {
            match fence.GetCompletedValue() {
                u64::MAX => {
                    device.GetDeviceRemovedReason()?;
                    return Err("device removed".into());
                }
                1 => break,
                _ if start.elapsed() >= Duration::from_secs(10) => {
                    return Err("GPU fence timed out".into());
                }
                _ => std::thread::sleep(Duration::from_millis(1)),
            }
        }
        readback.Map(
            0,
            Some(&D3D12_RANGE {
                Begin: 0,
                End: size as usize,
            }),
            Some(&mut mapped),
        )?;
        let pixels = std::slice::from_raw_parts(mapped.cast::<u8>(), size as usize);
        let matches = (0..2).all(|y| {
            (0..2).all(|x| {
                let start = y * 256 + x * 16;
                let expected = if y == 0 { COLOR } else { [0.; 4] };
                pixels[start..start + 16]
                    .chunks_exact(4)
                    .zip(expected)
                    .all(|(raw, v)| raw == v.to_le_bytes())
            })
        });
        readback.Unmap(0, Some(&D3D12_RANGE::default()));
        if !matches {
            return Err(
                "the 2x2 RGBA32F readback disagrees with the expected coverage or colors".into(),
            );
        }
    }
    Ok(())
}
