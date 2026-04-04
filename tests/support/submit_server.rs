#![allow(dead_code)]

use std::fs;
use std::io::{BufRead, BufReader, Read, Write};
use std::net::{SocketAddr, TcpListener, TcpStream};
use std::path::Path;
use std::sync::atomic::{AtomicBool, Ordering};
use std::sync::{Arc, Mutex};
use std::thread::{self, JoinHandle};
use std::time::Duration;

#[derive(Clone, Copy)]
pub enum Scenario {
  Success,
  SuccessMetaToken,
  SuccessLiveStatus,
  CompileErrorLiveStatus,
  DetailFailure,
  RuntimeError,
  LoggedOut,
  LoggedOutMetaToken,
  MissingCsrf,
  Timeout,
}

#[derive(Default)]
struct ServerState {
  submit_count: usize,
  requests: Vec<RequestRecord>,
}

#[derive(Clone)]
struct RequestRecord {
  method: String,
  body: String,
}

pub struct TestServer {
  addr: SocketAddr,
  pub base_url: String,
  state: Arc<Mutex<ServerState>>,
  stop: Arc<AtomicBool>,
  handle: Option<JoinHandle<()>>,
}

impl TestServer {
  pub fn spawn(scenario: Scenario) -> Self {
    let listener = TcpListener::bind("127.0.0.1:0").unwrap();
    listener.set_nonblocking(true).unwrap();
    let addr = listener.local_addr().unwrap();
    let state = Arc::new(Mutex::new(ServerState::default()));
    let stop = Arc::new(AtomicBool::new(false));
    let thread_state = Arc::clone(&state);
    let thread_stop = Arc::clone(&stop);
    let handle = thread::spawn(move || {
      loop {
        if thread_stop.load(Ordering::Relaxed) {
          break;
        }

        match listener.accept() {
          Ok((mut stream, _)) => {
            handle_connection(&mut stream, scenario, &thread_state)
          }
          Err(err) if err.kind() == std::io::ErrorKind::WouldBlock => {
            thread::sleep(Duration::from_millis(10));
          }
          Err(_) => break,
        }
      }
    });

    Self {
      addr,
      base_url: format!("http://{addr}"),
      state,
      stop,
      handle: Some(handle),
    }
  }

  pub fn submit_body(&self) -> String {
    self
      .state
      .lock()
      .unwrap()
      .requests
      .iter()
      .find(|record| record.method == "POST")
      .map(|record| record.body.clone())
      .unwrap_or_default()
  }
}

impl Drop for TestServer {
  fn drop(&mut self) {
    self.stop.store(true, Ordering::Relaxed);
    let _ = TcpStream::connect(self.addr);
    if let Some(handle) = self.handle.take() {
      let _ = handle.join();
    }
  }
}

pub fn write_config(
  dir: &Path,
  base_url: &str,
  timeout_secs: f64,
  poll_interval_secs: f64,
  cookie: &str,
) {
  fs::write(
    dir.join("yrs.toml"),
    format!(
      concat!(
        "library_root = \"{library_root}\"\n",
        "catalog_root = \".wait\"\n",
        "record_root = \"record\"\n",
        "summary_file = \"summary.md\"\n",
        "\n",
        "[submit]\n",
        "base_url = \"{base_url}\"\n",
        "cookie = \"{cookie}\"\n",
        "timeout_secs = {timeout_secs}\n",
        "poll_interval_secs = {poll_interval_secs}\n"
      ),
      library_root = dir.join("YRS").display(),
      base_url = base_url,
      cookie = cookie,
      timeout_secs = timeout_secs,
      poll_interval_secs = poll_interval_secs,
    ),
  )
  .unwrap();
}

fn handle_connection(
  stream: &mut TcpStream,
  scenario: Scenario,
  state: &Arc<Mutex<ServerState>>,
) {
  let mut reader = BufReader::new(stream.try_clone().unwrap());
  let mut request_line = String::new();
  if reader.read_line(&mut request_line).is_err() || request_line.is_empty() {
    return;
  }

  let mut content_length = 0usize;
  loop {
    let mut line = String::new();
    if reader.read_line(&mut line).is_err() {
      return;
    }
    if line == "\r\n" {
      break;
    }
    let lower = line.to_ascii_lowercase();
    if let Some(value) = lower.strip_prefix("content-length:") {
      content_length = value.trim().parse::<usize>().unwrap();
    }
  }

  let mut body = vec![0; content_length];
  if reader.read_exact(&mut body).is_err() {
    return;
  }

  let mut parts = request_line.split_whitespace();
  let method = parts.next().unwrap_or_default().to_string();
  let path = parts.next().unwrap_or_default().to_string();
  let body = String::from_utf8(body).unwrap();

  let mut locked = state.lock().unwrap();
  locked.requests.push(RequestRecord {
    method: method.clone(),
    body: body.clone(),
  });

  let (status, content_type, response_body) =
    route_request(&method, &path, scenario, &mut locked);
  drop(locked);

  write_response(stream, status, content_type, &response_body);
}

fn route_request(
  method: &str,
  path: &str,
  scenario: Scenario,
  state: &mut ServerState,
) -> (&'static str, &'static str, String) {
  match (method, path) {
    ("GET", "/problem/9584") => (
      "200 OK",
      "text/html; charset=utf-8",
      problem_page_html(scenario),
    ),
    ("GET", "/problem/status") => (
      "200 OK",
      "text/html; charset=utf-8",
      status_page_html(scenario, state.submit_count > 0),
    ),
    ("POST", "/problem/submit/9584") => {
      state.submit_count += 1;
      (
        "200 OK",
        "text/html; charset=utf-8",
        "submitted".to_string(),
      )
    }
    ("GET", "/ajax/judge-detail/101") => match scenario {
      Scenario::Success
      | Scenario::SuccessMetaToken
      | Scenario::SuccessLiveStatus => (
        "200 OK",
        "application/json",
        r#"{"code":0,"data":"All tests passed"}"#.to_string(),
      ),
      Scenario::CompileErrorLiveStatus => (
        "200 OK",
        "application/json",
        r#"{"code":0,"data":"line 1: expected ';'"}"#.to_string(),
      ),
      Scenario::DetailFailure => {
        ("200 OK", "application/json", "not-json".to_string())
      }
      _ => (
        "200 OK",
        "application/json",
        r#"{"code":0,"data":"Not available"}"#.to_string(),
      ),
    },
    ("GET", "/ajax/compile-error/101") => match scenario {
      Scenario::DetailFailure => (
        "200 OK",
        "application/json",
        r#"{"code":0,"data":"line 1: expected ';'"}"#.to_string(),
      ),
      _ => (
        "200 OK",
        "application/json",
        r#"{"code":0,"data":"Not available"}"#.to_string(),
      ),
    },
    ("GET", "/ajax/runtime-error/101") => match scenario {
      Scenario::RuntimeError => (
        "200 OK",
        "application/json",
        r#"{"code":0,"data":"Segmentation fault"}"#.to_string(),
      ),
      _ => (
        "200 OK",
        "application/json",
        r#"{"code":0,"data":"Not available"}"#.to_string(),
      ),
    },
    _ => ("404 Not Found", "text/plain", "missing".to_string()),
  }
}

fn write_response(
  stream: &mut TcpStream,
  status: &str,
  content_type: &str,
  body: &str,
) {
  let response = format!(
    concat!(
      "HTTP/1.1 {status}\r\n",
      "Content-Type: {content_type}\r\n",
      "Content-Length: {content_length}\r\n",
      "Connection: close\r\n",
      "\r\n",
      "{body}"
    ),
    status = status,
    content_type = content_type,
    content_length = body.len(),
    body = body,
  );
  stream.write_all(response.as_bytes()).unwrap();
  stream.flush().unwrap();
}

fn problem_page_html(scenario: Scenario) -> String {
  let token = match scenario {
    Scenario::MissingCsrf => String::new(),
    Scenario::SuccessMetaToken | Scenario::LoggedOutMetaToken => String::new(),
    _ => r#"<input type="hidden" name="_token" value="csrf123">"#.to_string(),
  };
  let meta_token = match scenario {
    Scenario::SuccessMetaToken | Scenario::LoggedOutMetaToken => {
      r#"<meta name="csrf-token" content="csrf-meta-123">"#.to_string()
    }
    _ => String::new(),
  };
  let account = match scenario {
    Scenario::LoggedOut | Scenario::LoggedOutMetaToken => String::new(),
    _ => concat!(
      r##"<a href="#" class="dropdown-toggle">"##,
      "Tester",
      r##"<span class="caret"></span></a>"##,
      "<strong>tester@example.com</strong>"
    )
    .to_string(),
  };

  format!(
    concat!(
      "<html><body>",
      "{meta_token}",
      "{token}",
      r#"<select name="lang">"#,
      r#"<option value="54" selected>GNU C++ 11.4.0</option>"#,
      r#"<option value="71">Rust 1.89</option>"#,
      "</select>",
      "{account}",
      "</body></html>"
    ),
    meta_token = meta_token,
    token = token,
    account = account,
  )
}

fn status_page_html(scenario: Scenario, submitted: bool) -> String {
  let mut rows = vec![status_row_html(
    100,
    "SomeoneElse",
    1000,
    "Old",
    "GNU C++ 11.4.0",
    "Accepted",
    "100",
    "15 ms",
    "1024 KB",
  )];

  if submitted {
    match scenario {
      Scenario::Success | Scenario::SuccessMetaToken => {
        rows.push(status_row_html(
          101,
          "Tester",
          9584,
          "Chosen",
          "GNU C++ 11.4.0",
          "Accepted",
          "100",
          "31 ms",
          "4096 KB",
        ))
      }
      Scenario::DetailFailure => rows.push(status_row_html(
        101,
        "Tester",
        9584,
        "Chosen",
        "GNU C++ 11.4.0",
        "Compile error",
        "0",
        "0 ms",
        "2048 KB",
      )),
      Scenario::RuntimeError => rows.push(status_row_html(
        101,
        "Tester",
        9584,
        "Chosen",
        "GNU C++ 11.4.0",
        "Runtime error",
        "0",
        "31 ms",
        "4096 KB",
      )),
      Scenario::Timeout => rows.push(status_row_html(
        101,
        "Tester",
        9584,
        "Chosen",
        "GNU C++ 11.4.0",
        "Judging",
        "0",
        "0 ms",
        "0 KB",
      )),
      Scenario::SuccessLiveStatus => rows.push(status_row_html_live(
        101, "Tester", 9584, "Chosen", "GNU C++", "Accepted", "100/100",
        "31 ms", "4096 KB",
      )),
      Scenario::CompileErrorLiveStatus => rows.push(status_row_html_live(
        101,
        "Tester",
        9584,
        "Chosen",
        "GNU C++",
        "Compilation error",
        "0/100",
        "0 ms",
        "2048 KB",
      )),
      Scenario::LoggedOut
      | Scenario::LoggedOutMetaToken
      | Scenario::MissingCsrf => {}
    }
  }

  format!("<table>{}</table>", rows.join(""))
}

fn status_row_html(
  run_id: u32,
  user_name: &str,
  problem_id: u32,
  problem_title: &str,
  language: &str,
  verdict: &str,
  grade: &str,
  time_text: &str,
  memory_text: &str,
) -> String {
  format!(
    concat!(
      r#"<tr data-source="{run_id}">"#,
      "<td>{run_id}</td>",
      "<td>2026-04-04 21:00</td>",
      "<td>{user_name}</td>",
      r#"<td><a href="/problem/{problem_id}">{problem_title}</a></td>"#,
      "<td>{language}</td>",
      "<td>128</td>",
      r#"<td class="verdict">{verdict}</td>"#,
      "<td>-</td>",
      r#"<td><a data-judge-detail="{run_id}">{grade}</a></td>"#,
      "<td>{time_text}</td>",
      "<td>{memory_text}</td>",
      "</tr>"
    ),
    run_id = run_id,
    user_name = user_name,
    problem_id = problem_id,
    problem_title = problem_title,
    language = language,
    verdict = verdict,
    grade = grade,
    time_text = time_text,
    memory_text = memory_text,
  )
}

fn status_row_html_live(
  run_id: u32,
  user_name: &str,
  problem_id: u32,
  problem_title: &str,
  language: &str,
  verdict: &str,
  grade: &str,
  time_text: &str,
  memory_text: &str,
) -> String {
  let verdict_html = match verdict {
    "Compilation error" => format!(
      r#"<a href="javascript:;" data-compile-error="{run_id}" class="verdict-compile-error">{verdict}</a>"#
    ),
    _ => verdict.to_string(),
  };

  format!(
    concat!(
      "<tr>",
      r#"<td><a href="javascript:;" data-source="{run_id}" data-lang="cpp">{run_id}</a></td>"#,
      "<td>2026-04-04 21:00</td>",
      r#"<td><a href="/problem/status/26043">{user_name}</a></td>"#,
      r#"<td class="text-inline" style="max-width: 250px;"><a href="/problem/{problem_id}">{problem_id} - {problem_title}</a></td>"#,
      "<td>{language}</td>",
      "<td>128/9999B</td>",
      r#"<td class="verdict verdict-rejected">{verdict_html}</td>"#,
      "<td>1.00</td>",
      r#"<td style="text-align:center"><a href="javascript:;" data-judge-detail="{run_id}">{grade}</a></td>"#,
      "<td>{time_text}</td>",
      "<td>{memory_text}</td>",
      "</tr>"
    ),
    run_id = run_id,
    user_name = user_name,
    problem_id = problem_id,
    problem_title = problem_title,
    language = language,
    verdict_html = verdict_html,
    grade = grade,
    time_text = time_text,
    memory_text = memory_text,
  )
}
