use std::ffi::{CString, c_char, c_int};
use std::process::ExitCode;

unsafe extern "C" {
    fn browser_run(page: *const c_char, address: *const c_char, smoke: c_int) -> c_int;
}

#[repr(i32)]
enum Action {
    Ignore,
    Up,
    Down,
    Left,
    Right,
    Follow,
    Back,
    Forward,
    Address,
    Reload,
    Delete,
}

#[unsafe(no_mangle)]
extern "C" fn browser_button(code: u16, swap: bool, on_start: bool) -> c_int {
    let action = match code {
        103 | 0x220 => Action::Up,
        108 | 0x221 => Action::Down,
        105 | 0x222 => Action::Left,
        106 | 0x223 => Action::Right,
        0x131 if !swap => Action::Follow,
        0x130 if swap => Action::Follow,
        0x131 | 0x130 if on_start => Action::Delete,
        0x131 | 0x130 => Action::Back,
        0x133 => Action::Address,
        0x134 => Action::Reload,
        0x136 => Action::Back,
        0x137 => Action::Forward,
        _ => Action::Ignore,
    };
    action as c_int
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
    let page = c"";
    let start_page = CString::new(include_str!("../ui/start.html")).expect("start page has no NUL");
    let result = unsafe {
        browser_run(
            start_page.as_ptr(),
            address.as_ref().map_or(page.as_ptr(), |uri| uri.as_ptr()),
            i32::from(smoke),
        )
    };
    if result == 0 { ExitCode::SUCCESS } else { ExitCode::FAILURE }
}
