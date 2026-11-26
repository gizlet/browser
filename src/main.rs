use std::ffi::{CString, c_char, c_int};
use std::process::ExitCode;

unsafe extern "C" {
    fn browser_run(page: *const c_char, overlay: *const c_char, address: *const c_char, smoke: c_int) -> c_int;
}

#[repr(i32)]
enum Button {
    None,
    Up,
    Down,
    Left,
    Right,
    A,
    B,
    X,
    Y,
    L1,
    R1,
    L2,
    R2,
    Select,
    Start,
}

const BTN_SOUTH: u16 = 0x130;
const BTN_EAST: u16 = 0x131;
const BTN_NORTH: u16 = 0x133;
const BTN_WEST: u16 = 0x134;

#[unsafe(no_mangle)]
extern "C" fn browser_button(code: u16, swap: bool) -> c_int {
    let (a, b) = if swap { (BTN_SOUTH, BTN_EAST) } else { (BTN_EAST, BTN_SOUTH) };
    let button = match code {
        0x220 => Button::Up,
        0x221 => Button::Down,
        0x222 => Button::Left,
        0x223 => Button::Right,
        code if code == a => Button::A,
        code if code == b => Button::B,
        BTN_NORTH => Button::X,
        BTN_WEST => Button::Y,
        0x136 => Button::L1,
        0x137 => Button::R1,
        0x138 => Button::L2,
        0x139 => Button::R2,
        0x13a => Button::Select,
        0x13b => Button::Start,
        _ => Button::None,
    };
    button as c_int
}

fn main() -> ExitCode {
    let mut args = std::env::args().skip(1);
    let first = args.next();
    if args.next().is_some() {
        eprintln!("Usage: gizlet-browser [URL | --smoke]");
        return ExitCode::FAILURE;
    }
    let smoke = first.as_deref() == Some("--smoke");
    let address = first.filter(|_| !smoke).and_then(|value| CString::new(value).ok());
    let start_page = CString::new(include_str!("../ui/start.html")).expect("start page has no NUL");
    let overlay = CString::new(include_str!("../ui/overlay.js")).expect("overlay has no NUL");
    let result = unsafe {
        browser_run(
            start_page.as_ptr(),
            overlay.as_ptr(),
            address.as_ref().map_or(c"".as_ptr(), |uri| uri.as_ptr()),
            i32::from(smoke),
        )
    };
    if result == 0 { ExitCode::SUCCESS } else { ExitCode::FAILURE }
}
