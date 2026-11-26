use windows::{
    Win32::Graphics::{
        Direct3D::D3D_FEATURE_LEVEL_12_0,
        Direct3D12::{D3D12CreateDevice, ID3D12Device},
        Dxgi::*,
    },
    core::Result,
};

/// Approach after XCL 2dd04da: keep the DXGI high-performance order and skip
/// software devices. The desktop surface stops after its first usable device.
pub fn hardware(
    factory: &IDXGIFactory6,
) -> impl Iterator<Item = Result<(IDXGIAdapter1, DXGI_ADAPTER_DESC1)>> + '_ {
    let mut indices = 0..;
    std::iter::from_fn(move || {
        loop {
            let index = indices.next()?;
            let adapter: IDXGIAdapter1 = match unsafe {
                factory.EnumAdapterByGpuPreference(index, DXGI_GPU_PREFERENCE_HIGH_PERFORMANCE)
            } {
                Ok(adapter) => adapter,
                Err(e) if e.code() == DXGI_ERROR_NOT_FOUND => return None,
                Err(e) => return Some(Err(e)),
            };
            match unsafe { adapter.GetDesc1() } {
                Ok(desc) if desc.Flags & DXGI_ADAPTER_FLAG_SOFTWARE.0 as u32 == 0 => {
                    return Some(Ok((adapter, desc)));
                }
                Ok(_) => continue,
                Err(e) => return Some(Err(e)),
            }
        }
    })
}

pub fn device(adapter: &IDXGIAdapter1) -> Option<ID3D12Device> {
    let mut device = None;
    unsafe { D3D12CreateDevice(adapter, D3D_FEATURE_LEVEL_12_0, &mut device) }.ok()?;
    device
}

pub fn name(desc: &DXGI_ADAPTER_DESC1) -> String {
    let end = desc
        .Description
        .iter()
        .position(|&c| c == 0)
        .unwrap_or(desc.Description.len());
    String::from_utf16_lossy(&desc.Description[..end])
}
