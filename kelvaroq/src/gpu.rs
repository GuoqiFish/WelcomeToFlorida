use anyhow::{Context, Result, anyhow, bail, ensure};
use std::{
    mem::ManuallyDrop,
    ptr,
    time::{Duration, Instant},
};
use windows::{
    Win32::Foundation::HWND,
    Win32::Graphics::{
        Direct3D::D3D_FEATURE_LEVEL_12_0,
        Direct3D12::*,
        Dxgi::{Common::*, *},
    },
    core::Interface,
};

pub const OUTPUT_WIDTH: u32 = 3840;
pub const OUTPUT_HEIGHT: u32 = 2160;

/// The swap chain owns a reference to its direct queue. Submission will begin
/// when the guest supplies a display buffer.
pub struct DisplaySurface {
    _chain: IDXGISwapChain1,
    pub adapter_name: String,
}

impl DisplaySurface {
    pub fn open(window: HWND) -> Result<Self> {
        // SAFETY: the window belongs to the UI thread and outlives this surface.
        unsafe {
            let factory: IDXGIFactory6 = CreateDXGIFactory1()?;
            for entry in crate::adapters::hardware(&factory) {
                let (adapter, info) = entry?;
                let Some(device) = crate::adapters::device(&adapter) else {
                    continue;
                };
                let queue: ID3D12CommandQueue =
                    device.CreateCommandQueue(&D3D12_COMMAND_QUEUE_DESC::default())?;
                let desc = DXGI_SWAP_CHAIN_DESC1 {
                    Width: OUTPUT_WIDTH,
                    Height: OUTPUT_HEIGHT,
                    Format: DXGI_FORMAT_R8G8B8A8_UNORM,
                    SampleDesc: DXGI_SAMPLE_DESC {
                        Count: 1,
                        Quality: 0,
                    },
                    BufferUsage: DXGI_USAGE_RENDER_TARGET_OUTPUT,
                    BufferCount: 2,
                    SwapEffect: DXGI_SWAP_EFFECT_FLIP_DISCARD,
                    Scaling: DXGI_SCALING_STRETCH,
                    AlphaMode: DXGI_ALPHA_MODE_IGNORE,
                    ..Default::default()
                };
                let chain = factory.CreateSwapChainForHwnd(&queue, window, &desc, None, None)?;
                for i in 0..desc.BufferCount {
                    let buffer: ID3D12Resource = chain.GetBuffer(i)?;
                    let actual = buffer.GetDesc();
                    ensure!(
                        actual.Width == u64::from(OUTPUT_WIDTH) && actual.Height == OUTPUT_HEIGHT,
                        "dimensions DXGI inattendues"
                    );
                }
                factory.MakeWindowAssociation(window, DXGI_MWA_NO_ALT_ENTER)?;
                return Ok(Self {
                    _chain: chain,
                    adapter_name: crate::adapters::name(&info),
                });
            }
        }
        bail!("aucun adaptateur matériel compatible Direct3D 12")
    }
}

pub fn probe(selected: Option<u32>) -> Result<()> {
    // SAFETY: DXGI owns enumeration and all COM interfaces below are reference counted.
    let factory: IDXGIFactory1 = unsafe { CreateDXGIFactory1()? };
    let mut tested = 0;
    let mut errors = Vec::new();
    for index in 0.. {
        let adapter = match unsafe { factory.EnumAdapters1(index) } {
            Ok(adapter) => adapter,
            Err(e) if e.code() == DXGI_ERROR_NOT_FOUND => break,
            Err(e) => return Err(e.into()),
        };
        let desc = unsafe { adapter.GetDesc1()? };
        if selected.is_some_and(|i| i != index)
            || desc.Flags & DXGI_ADAPTER_FLAG_SOFTWARE.0 as u32 != 0
        {
            continue;
        }
        let name = crate::adapters::name(&desc);
        tested += 1;
        match verify(&adapter) {
            Ok(()) => println!(
                "[{index}] {name} ({:04x}:{:04x}) D3D12 two frames, 8/8 RGBA pixels verified",
                desc.VendorId, desc.DeviceId
            ),
            Err(e) => errors.push(format!("[{index}] {name}: {e:#}")),
        }
    }
    ensure!(tested > 0, "no matching hardware adapter");
    ensure!(errors.is_empty(), "{}", errors.join("\n"));
    Ok(())
}

fn verify(adapter: &IDXGIAdapter1) -> Result<()> {
    let mut device = None;
    // SAFETY: valid adapter and output slot. Every handle stays alive through its fence.
    unsafe {
        D3D12CreateDevice(adapter, D3D_FEATURE_LEVEL_12_0, &mut device)?;
    }
    let device: ID3D12Device = device.context("D3D12 returned no device")?;
    let queue: ID3D12CommandQueue =
        unsafe { device.CreateCommandQueue(&D3D12_COMMAND_QUEUE_DESC::default())? };
    let fence: ID3D12Fence = unsafe { device.CreateFence(0, D3D12_FENCE_FLAG_NONE)? };
    for (i, colour) in [[255u8, 0, 0, 255], [0, 255, 0, 255]]
        .into_iter()
        .enumerate()
    {
        frame(&device, &queue, &fence, i as u64 + 1, colour)?;
    }
    Ok(())
}

fn resource(
    device: &ID3D12Device,
    heap: D3D12_HEAP_TYPE,
    desc: &D3D12_RESOURCE_DESC,
    state: D3D12_RESOURCE_STATES,
) -> Result<ID3D12Resource> {
    let props = D3D12_HEAP_PROPERTIES {
        Type: heap,
        CreationNodeMask: 1,
        VisibleNodeMask: 1,
        ..Default::default()
    };
    let mut res = None;
    // SAFETY: the heap and descriptor live until the synchronous creation returns.
    unsafe {
        device.CreateCommittedResource(
            &props,
            D3D12_HEAP_FLAG_NONE,
            desc,
            state,
            None,
            &mut res,
        )?;
    }
    res.context("D3D12 returned no resource")
}

fn frame(
    device: &ID3D12Device,
    queue: &ID3D12CommandQueue,
    fence: &ID3D12Fence,
    token: u64,
    rgba: [u8; 4],
) -> Result<()> {
    let tex_desc = D3D12_RESOURCE_DESC {
        Dimension: D3D12_RESOURCE_DIMENSION_TEXTURE2D,
        Width: 2,
        Height: 2,
        DepthOrArraySize: 1,
        MipLevels: 1,
        Format: DXGI_FORMAT_R8G8B8A8_UNORM,
        SampleDesc: DXGI_SAMPLE_DESC {
            Count: 1,
            Quality: 0,
        },
        Flags: D3D12_RESOURCE_FLAG_ALLOW_RENDER_TARGET,
        ..Default::default()
    };
    let tex = resource(
        device,
        D3D12_HEAP_TYPE_DEFAULT,
        &tex_desc,
        D3D12_RESOURCE_STATE_RENDER_TARGET,
    )?;
    let mut footprint = D3D12_PLACED_SUBRESOURCE_FOOTPRINT::default();
    let mut size = 0u64;
    unsafe {
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
    }
    ensure!(
        (264..=4096).contains(&size),
        "unexpected readback size {size}"
    );
    let buf_desc = D3D12_RESOURCE_DESC {
        Dimension: D3D12_RESOURCE_DIMENSION_BUFFER,
        Width: size,
        Height: 1,
        DepthOrArraySize: 1,
        MipLevels: 1,
        SampleDesc: DXGI_SAMPLE_DESC {
            Count: 1,
            Quality: 0,
        },
        Layout: D3D12_TEXTURE_LAYOUT_ROW_MAJOR,
        ..Default::default()
    };
    let readback = resource(
        device,
        D3D12_HEAP_TYPE_READBACK,
        &buf_desc,
        D3D12_RESOURCE_STATE_COPY_DEST,
    )?;
    let hd = D3D12_DESCRIPTOR_HEAP_DESC {
        Type: D3D12_DESCRIPTOR_HEAP_TYPE_RTV,
        NumDescriptors: 1,
        ..Default::default()
    };
    // SAFETY: matching resource types, valid footprints and a list private to this frame.
    unsafe {
        let heap: ID3D12DescriptorHeap = device.CreateDescriptorHeap(&hd)?;
        let rtv = heap.GetCPUDescriptorHandleForHeapStart();
        device.CreateRenderTargetView(&tex, None, rtv);
        let alloc: ID3D12CommandAllocator =
            device.CreateCommandAllocator(D3D12_COMMAND_LIST_TYPE_DIRECT)?;
        let list: ID3D12GraphicsCommandList =
            device.CreateCommandList(0, D3D12_COMMAND_LIST_TYPE_DIRECT, &alloc, None)?;
        list.ClearRenderTargetView(rtv, &rgba.map(|c| f32::from(c) / 255.0), None);
        let transition = D3D12_RESOURCE_TRANSITION_BARRIER {
            pResource: ManuallyDrop::new(Some(tex.clone())),
            Subresource: D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES,
            StateBefore: D3D12_RESOURCE_STATE_RENDER_TARGET,
            StateAfter: D3D12_RESOURCE_STATE_COPY_SOURCE,
        };
        let mut barrier = D3D12_RESOURCE_BARRIER {
            Type: D3D12_RESOURCE_BARRIER_TYPE_TRANSITION,
            Flags: D3D12_RESOURCE_BARRIER_FLAG_NONE,
            Anonymous: D3D12_RESOURCE_BARRIER_0 {
                Transition: ManuallyDrop::new(transition),
            },
        };
        list.ResourceBarrier(std::slice::from_ref(&barrier));
        let transition = &mut *barrier.Anonymous.Transition;
        ManuallyDrop::drop(&mut transition.pResource);
        let mut src = D3D12_TEXTURE_COPY_LOCATION {
            pResource: ManuallyDrop::new(Some(tex.clone())),
            Type: D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX,
            Anonymous: D3D12_TEXTURE_COPY_LOCATION_0 {
                SubresourceIndex: 0,
            },
        };
        let mut dst = D3D12_TEXTURE_COPY_LOCATION {
            pResource: ManuallyDrop::new(Some(readback.clone())),
            Type: D3D12_TEXTURE_COPY_TYPE_PLACED_FOOTPRINT,
            Anonymous: D3D12_TEXTURE_COPY_LOCATION_0 {
                PlacedFootprint: footprint,
            },
        };
        list.CopyTextureRegion(&dst, 0, 0, 0, &src, None);
        ManuallyDrop::drop(&mut src.pResource);
        ManuallyDrop::drop(&mut dst.pResource);
        list.Close()?;
        queue.ExecuteCommandLists(&[Some(list.cast()?)]);
        queue.Signal(fence, token)?;
        let deadline = Instant::now() + Duration::from_secs(10);
        loop {
            let completed = fence.GetCompletedValue();
            if completed == u64::MAX {
                device.GetDeviceRemovedReason()?;
                bail!("device removed");
            }
            if completed >= token {
                break;
            }
            if Instant::now() >= deadline {
                bail!("GPU fence timed out for token {token}");
            }
            std::thread::sleep(Duration::from_millis(1));
        }
        let range = D3D12_RANGE {
            Begin: 0,
            End: size as usize,
        };
        let mut mapped = ptr::null_mut();
        readback.Map(0, Some(&range), Some(&mut mapped))?;
        let pixels = std::slice::from_raw_parts(mapped as *const u8, size as usize);
        let matched = (0..2).all(|y| {
            (0..2).all(|x| {
                let start =
                    footprint.Offset as usize + y * (footprint.Footprint.RowPitch as usize) + x * 4;
                pixels.get(start..start + 4) == Some(&rgba)
            })
        });
        readback.Unmap(0, Some(&D3D12_RANGE { Begin: 0, End: 0 }));
        matched
            .then_some(())
            .ok_or_else(|| anyhow!("pixel mismatch for token {token}"))
    }
}
