// Host test for fetchResumable (ResumableFetch.h) over scripted HTTP replies.
#include <ResumableFetch.h>

#include <cassert>
#include <cstdio>

// SecureClient is only reached for https, which these tests avoid (the
// redirect test proves an https target was stepped down to http).
namespace freeink {
SecureClient::~SecureClient() = default;
void SecureClient::setCACert(const char*) {}
void SecureClient::setInsecure() {}
int SecureClient::connect(IPAddress, uint16_t) { return 0; }
int SecureClient::connect(const char*, uint16_t) { return 0; }
size_t SecureClient::write(uint8_t) { return 0; }
size_t SecureClient::write(const uint8_t*, size_t) { return 0; }
int SecureClient::available() { return 0; }
int SecureClient::read() { return -1; }
int SecureClient::read(uint8_t*, size_t) { return -1; }
int SecureClient::peek() { return -1; }
void SecureClient::flush() {}
void SecureClient::stop() {}
uint8_t SecureClient::connected() { return 0; }
bool SecureClient::tls13Available() { return false; }
}  // namespace freeink

using freeink::FetchOptions;
using freeink::FetchResult;
using freeink::FetchSink;

namespace {
std::string body;
int rewinds = 0;

void reset(std::initializer_list<const char*> replies) {
  FakeNet::replies().assign(replies.begin(), replies.end());
  FakeNet::requests().clear();
  FakeNet::hosts().clear();
  body.clear();
  rewinds = 0;
}

FetchSink sink(bool canRewind = true) {
  FetchSink s;
  s.write = [](const uint8_t* d, size_t n) {
    body.append(reinterpret_cast<const char*>(d), n);
    return true;
  };
  if (canRewind) {
    s.rewind = [] {
      ++rewinds;
      body.clear();
      return true;
    };
  }
  return s;
}

FetchResult get(const char* url, const FetchSink& s, FetchOptions o = {}) {
  return freeink::fetchResumable(
      url, o,
      [](freeink::SecureHttpClient& h, bool sameOrigin) {
        h.setTimeout(1000);
        if (sameOrigin) h.addHeader("Authorization", "Bearer secret");
      },
      s);
}

bool sentAuth(size_t i) { return FakeNet::requests()[i].find("Authorization:") != std::string::npos; }

bool sentRange(size_t i, const char* range) {
  return FakeNet::requests()[i].find(std::string("Range: ") + range) != std::string::npos;
}

std::string responseWithFields(size_t count) {
  assert(count > 0);
  std::string response = "HTTP/1.1 200 OK\r\n";
  for (size_t i = 1; i < count; ++i) response += "X-Repeated: value\r\n";
  return response + "Content-Length: 1\r\n\r\nZ";
}

std::string responseWithHeaderBytes(size_t bytes, bool malformed = false) {
  std::string response = "HTTP/1.1 200 OK\r\nContent-Length: 1\r\n";
  // Individually short fields make the aggregate bound, not MAX_LINE, decide
  // whether this reply is accepted. Optionally use ignored colon-less lines.
  const std::string name = malformed ? "xxx" : "X: ";
  for (size_t i = 0; i < 4; ++i) response += name + std::string(3995, 'x') + "\r\n";
  const size_t remaining = bytes - response.size() - 2;
  assert(remaining >= 5 && remaining < 4096);
  response += name + std::string(remaining - 5, 'x') + "\r\n\r\n";
  assert(response.size() == bytes);
  return response + "Z";
}

void headerLimitTests() {
  for (size_t fields : {size_t{64}, size_t{65}}) {
    const auto response = responseWithFields(fields);
    reset({response.c_str()});
    freeink::SecureHttpClient http;
    assert(http.begin("http://a/headers"));
    size_t delivered = 0;
    const int status = http.GET([&](const uint8_t*, size_t size) {
      delivered += size;
      return true;
    });
    assert((fields == 64) ? status == 200 && delivered == 1 && http.responseComplete()
                          : status == -1 && delivered == 0 && !http.responseComplete());
    assert(http.getHeaders().size() == 64);
    assert(FakeNet::requests().size() == 1);  // A limit rejection never retries itself.
  }
  for (bool malformed : {false, true}) {
    for (size_t bytes : {size_t{16384}, size_t{16385}}) {
      const auto response = responseWithHeaderBytes(bytes, malformed);
      reset({response.c_str(), "HTTP/1.1 200 OK\r\nContent-Length: 2\r\n\r\nok"});
      freeink::SecureHttpClient http;
      assert(http.begin("http://a/headers"));
      size_t delivered = 0;
      const int status = http.GET([&](const uint8_t*, size_t size) {
        delivered += size;
        return true;
      });
      assert((bytes == 16384) ? status == 200 && delivered == 1 && http.responseComplete()
                              : status == -1 && delivered == 0 && !http.responseComplete());
      // The next request must use a clean connection and fresh header budget.
      assert(http.begin("http://a/recovery") && http.GET() == 200);
      assert(http.responseComplete() && http.getString() == "ok" && FakeNet::hosts().size() == 2);
      assert(http.getUniqueHeader("x") == nullptr);
    }
  }
  for (const auto& response :
       {std::string("HTTP/1.1 200 OK\r\nContent-Length: 1\r\nX: ") + std::string(4097, 'x') + "\r\n\r\nZ",
        std::string("HTTP/1.1 200 OK\r\nContent-Length: 1\r\nX-Truncated: unfinished")}) {
    reset({response.c_str()});
    freeink::SecureHttpClient http;
    assert(http.begin("http://a/headers"));
    bool delivered = false;
    assert(http.GET([&](const uint8_t*, size_t) {
      delivered = true;
      return true;
    }) == -1);
    assert(!delivered && !http.responseComplete());
  }
}
}  // namespace

int main() {
  headerLimitTests();
  // Borrow a unique header during streaming, with no header collection copies.
  reset({"HTTP/1.1 200 OK\r\nContent-Disposition: attachment; filename=Book.epub\r\nContent-Length: 3\r\n\r\nabc"});
  {
    freeink::SecureHttpClient http;
    assert(http.begin("http://a/f"));
    http.setReuse(false);
    bool checked = false;
    assert(http.GET([&](const uint8_t*, size_t) {
      const auto* value = http.getUniqueHeader("content-disposition");
      assert(value && *value == "attachment; filename=Book.epub");
      assert(http.getUniqueHeader("missing") == nullptr);
      assert(http.getUniqueHeader(nullptr) == nullptr);
      checked = true;
      return true;
    }) == 200);
    assert(checked);
    reset({"HTTP/1.1 200 OK\r\nContent-Length: 0\r\n\r\n"});
    assert(http.begin("http://a/g"));
    assert(http.GET() == 200);
    assert(http.getUniqueHeader("content-disposition") == nullptr);
  }
  for (const char* second : {"attachment; filename=Other.epub", "attachment; filename=Book.epub", ""}) {
    const std::string response = std::string("HTTP/1.1 200 OK\r\nContent-Disposition: attachment; filename=Book.epub\r\ncOnTeNt-DiSpOsItIoN: ") + second + "\r\nContent-Length: 0\r\n\r\n";
    reset({response.c_str()});
    freeink::SecureHttpClient http;
    assert(http.begin("http://a/f"));
    assert(http.GET() == 200);
    assert(http.getUniqueHeader("content-disposition") == nullptr);
  }

  // Drop mid-body, then a 206 continues from the received count.
  reset({"HTTP/1.1 200 OK\r\nContent-Length: 10\r\n\r\n01234",
         "HTTP/1.1 206 Partial Content\r\nContent-Range: bytes 5-9/10\r\nContent-Length: 5\r\n\r\n56789"});
  FetchResult r = get("http://a/f", sink());
  assert(r.complete && r.status == 206 && r.bytes == 10 && r.total == 10 && body == "0123456789");
  assert(!sentRange(0, "") && sentRange(1, "bytes=5-") && rewinds == 0);

  // Range ignored: the 200 restarts the body, so the sink rewinds first.
  reset({"HTTP/1.1 200 OK\r\nContent-Length: 10\r\n\r\n01234",
         "HTTP/1.1 200 OK\r\nContent-Length: 10\r\n\r\n0123456789"});
  r = get("http://a/f", sink());
  assert(r.complete && r.bytes == 10 && body == "0123456789" && rewinds == 1);

  // A sink that cannot rewind stops instead of appending a restarted body.
  reset({"HTTP/1.1 200 OK\r\nContent-Length: 10\r\n\r\n01234",
         "HTTP/1.1 200 OK\r\nContent-Length: 10\r\n\r\n0123456789"});
  r = get("http://a/f", sink(false));
  assert(r.stopped && !r.complete && body == "01234");

  // Continuing an earlier transfer asks for the rest on the first request.
  reset({"HTTP/1.1 206 Partial Content\r\nContent-Range: bytes 4-9/10\r\nContent-Length: 6\r\n\r\n456789"});
  FetchOptions from4;
  from4.startOffset = 4;
  r = get("http://a/f", sink(), from4);
  assert(r.complete && r.bytes == 10 && r.total == 10 && body == "456789" && sentRange(0, "bytes=4-"));

  // Redirects are followed, and redirectToHttp steps an https target down.
  reset({"HTTP/1.1 302 Found\r\nLocation: https://b/g\r\nContent-Length: 3\r\n\r\nxyz",
         "HTTP/1.1 200 OK\r\nContent-Length: 2\r\n\r\nok"});
  FetchOptions down;
  down.redirectToHttp = true;
  r = get("http://a/f", sink(), down);
  assert(r.complete && body == "ok" && FakeNet::hosts()[1] == "b" && FakeNet::requests()[1].find("GET /g ") == 0);
  // Credentials stay with the starting origin: sent to a, withheld from b.
  assert(sentAuth(0) && !sentAuth(1));

  // A same-origin redirect (another path on the same host) keeps them.
  reset({"HTTP/1.1 302 Found\r\nLocation: /other\r\nContent-Length: 0\r\n\r\n",
         "HTTP/1.1 200 OK\r\nContent-Length: 2\r\n\r\nok"});
  r = get("http://a/f", sink());
  assert(r.complete && sentAuth(0) && sentAuth(1));

  // Origin equality: default ports, case, and userinfo don't matter; scheme,
  // host, and port do (an https->http step-down is a different origin).
  assert(freeink::fetchOrigin("HTTPS://Host.Example/x") == freeink::fetchOrigin("https://host.example:443/y"));
  assert(freeink::fetchOrigin("http://user:pw@a:80/") == freeink::fetchOrigin("http://a"));
  assert(freeink::fetchOrigin("https://a/") != freeink::fetchOrigin("http://a/"));
  assert(freeink::fetchOrigin("https://a/") != freeink::fetchOrigin("https://a:8443/"));
  assert(freeink::fetchOrigin("http://[::1]/") == "http://[::1]:80");

  // Error bodies never reach the sink, and an http error is not retried.
  reset({"HTTP/1.1 404 Not Found\r\nContent-Length: 4\r\n\r\nnope", "HTTP/1.1 200 OK\r\nContent-Length: 2\r\n\r\nok"});
  r = get("http://a/f", sink());
  assert(r.status == 404 && !r.complete && body.empty() && FakeNet::requests().size() == 1);

  // Consecutive zero-progress attempts give up after maxStalled.
  reset({"HTTP/1.1 200 OK\r\nContent-Length: 10\r\n\r\n01",
         "HTTP/1.1 206 Partial Content\r\nContent-Range: bytes 2-9/10\r\nContent-Length: 8\r\n\r\n",
         "HTTP/1.1 206 Partial Content\r\nContent-Range: bytes 2-9/10\r\nContent-Length: 8\r\n\r\n",
         "HTTP/1.1 206 Partial Content\r\nContent-Range: bytes 2-9/10\r\nContent-Length: 8\r\n\r\n",
         "HTTP/1.1 206 Partial Content\r\nContent-Range: bytes 2-9/10\r\nContent-Length: 8\r\n\r\n23456789"});
  r = get("http://a/f", sink());
  assert(!r.complete && r.bytes == 2 && FakeNet::requests().size() == 4);

  // A complete partial response is not necessarily the whole resource.
  reset({"HTTP/1.1 200 OK\r\nContent-Length: 10\r\n\r\n01234",
         "HTTP/1.1 206 Partial Content\r\nContent-Range: bytes 5-7/10\r\nContent-Length: 3\r\n\r\n567",
         "HTTP/1.1 206 Partial Content\r\nContent-Range: bytes 8-9/10\r\nContent-Length: 2\r\n\r\n89"});
  r = get("http://a/f", sink());
  assert(r.complete && r.bytes == 10 && body == "0123456789" && sentRange(2, "bytes=8-"));

  // Invalid/missing ranges must not append anything or complete the file.
  for (const char* range :
       {"", "bytes 0-4/10", "bytes 6-9/10", "bytes 5-9/11", "bytes 5-8/10", "bytes 5-9/*", "bytes 5-9/9",
        "bytes 9-5/10", "bytes -5-9/10", "bytes 5-9/18446744073709551616", "bytes 5-9/10junk"}) {
    const std::string response =
        std::string("HTTP/1.1 206 Partial Content\r\nContent-Range: ") + range + "\r\nContent-Length: 5\r\n\r\n56789";
    reset({"HTTP/1.1 200 OK\r\nContent-Length: 10\r\n\r\n01234", response.c_str()});
    r = get("http://a/f", sink());
    assert(!r.complete && r.bytes == 5 && body == "01234");
  }

  // Reaching the end of a short range must not hide a later failure.
  reset({"HTTP/1.1 200 OK\r\nContent-Length: 10\r\n\r\n01234",
         "HTTP/1.1 206 Partial Content\r\nContent-Range: bytes 5-7/10\r\nContent-Length: 3\r\n\r\n567",
         "HTTP/1.1 503 Unavailable\r\nContent-Length: 0\r\n\r\n"});
  r = get("http://a/f", sink());
  assert(!r.complete && r.bytes == 8 && r.total == 10);

  // Chunked 206 bodies must agree with the advertised interval too.
  for (const char* chunked : {"3\r\n567\r\n0\r\n\r\n", "6\r\n567890\r\n0\r\n\r\n"}) {
    const std::string response = std::string("HTTP/1.1 206 Partial Content\r\nContent-Range: bytes 5-9/10\r\n") +
                                 "Transfer-Encoding: chunked\r\n\r\n" + chunked;
    reset({"HTTP/1.1 200 OK\r\nContent-Length: 10\r\n\r\n01234", response.c_str()});
    r = get("http://a/f", sink());
    assert(!r.complete);
  }

  // A restarted resource can change size, including becoming empty.
  reset({"HTTP/1.1 200 OK\r\nContent-Length: 10\r\n\r\n01234", "HTTP/1.1 200 OK\r\nContent-Length: 3\r\n\r\nabc"});
  r = get("http://a/f", sink());
  assert(r.complete && r.bytes == 3 && r.total == 3 && body == "abc" && rewinds == 1);
  reset({"HTTP/1.1 200 OK\r\nContent-Length: 10\r\n\r\n01234", "HTTP/1.1 200 OK\r\nContent-Length: 0\r\n\r\n"});
  r = get("http://a/f", sink());
  assert(r.complete && r.bytes == 0 && r.total == 0 && body.empty() && rewinds == 1);
  reset({"HTTP/1.1 200 OK\r\nContent-Length: 10\r\n\r\n01234",
         "HTTP/1.1 206 Partial Content\r\nContent-Length: 0\r\n\r\n"});
  r = get("http://a/f", sink());
  assert(!r.complete && r.bytes == 5);

  puts("resumable fetch ok");
}
