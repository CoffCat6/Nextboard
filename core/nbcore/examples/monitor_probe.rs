//! 临时诊断:打印显示器几何与整屏捕获尺寸,排查截图分辨率/多屏问题。
//! 用法:cd core && cargo run --example monitor_probe

fn main() {
    let list = nbcore::capture::monitors();
    println!("显示器数量: {}", list.len());
    for (i, m) in list.iter().enumerate() {
        println!(
            "  [{i}] x={} y={} w={} h={} scale={} primary={}",
            m.x, m.y, m.width, m.height, m.scale_factor, m.is_primary
        );
    }
    for i in 0..list.len() as u32 {
        match nbcore::capture::capture_monitor(i) {
            Ok(img) => {
                let px = img.width as f64 * img.height as f64;
                println!(
                    "capture[{i}] = {}x{} ({} 字节, {:.1} B/px)",
                    img.width,
                    img.height,
                    img.rgba.len(),
                    img.rgba.len() as f64 / px
                );
            }
            Err(err) => println!("capture[{i}] 失败: {err}"),
        }
    }
}
