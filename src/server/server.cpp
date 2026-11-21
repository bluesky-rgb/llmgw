// Server：启动与停止，以及 pregrow_fd_table。
#include "llmgw/server/server.h"

#include <fcntl.h>
#include <sys/resource.h>
#include <unistd.h>

#include <algorithm>
#include <thread>
#include <vector>

#include "control_plane.h"
#include "worker.h"

namespace llmgw::server {

using detail::tcp;

struct Server::Impl {
  ServerOptions options;
  detail::SharedState shared;
  std::vector<std::unique_ptr<detail::Worker>> workers;
  std::unique_ptr<detail::ControlPlane> control;
  bool started = false;
};

namespace {

// 解析监听地址（只在启动时执行，允许阻塞）
Result<tcp::endpoint> resolve(const std::string& host, std::uint16_t port,
                              const std::string& what) {
  boost::asio::io_context ioc;
  tcp::resolver resolver(ioc);
  boost::system::error_code ec;
  auto results = resolver.resolve(host, std::to_string(port),
                                  tcp::resolver::passive | tcp::resolver::numeric_service, ec);
  if (ec || results.empty())
    return fail(ErrorCode::kInternal, what + " 无法解析监听地址 \"" + host + "\"：" + ec.message());
  return results.begin()->endpoint();
}

}  // namespace

Server::Server(ServerOptions options) : impl_(std::make_unique<Impl>()) {
  impl_->options = std::move(options);
}

Server::~Server() { stop(); }

Result<void> Server::start() {
  auto& o = impl_->options;
  if (!o.config || !o.dispatcher_factory)
    return fail(ErrorCode::kInternal, "ServerOptions 缺少 config 或 dispatcher_factory");
  const auto& s = o.config->server;
  const unsigned n = s.workers > 0 ? s.workers : std::max(1U, std::thread::hardware_concurrency());

  auto data = resolve(s.host, s.port, "数据面（server.host/port）");
  if (!data) return fail(data.error());
  // 端口为 0 时，第一个 Worker 绑定后由系统分配端口，其余 Worker 绑定同一个端口
  for (unsigned i = 0; i < n; ++i) {
    auto w = std::make_unique<detail::Worker>(i, o.config, &impl_->shared, o.dispatcher_factory,
                                              o.metrics_factory);
    if (auto r = w->listen(*data); !r) {
      return fail(ErrorCode::kInternal, "数据面无法监听 " + s.host + ":" +
                                            std::to_string(data->port()) + "：" +
                                            r.error().message);
    }
    if (i == 0) data->port(w->port());
    impl_->workers.push_back(std::move(w));
  }
  auto admin = resolve(s.admin.host, s.admin.port, "管理面（server.admin_listen）");
  if (!admin) return fail(admin.error());
  auto obs =
      resolve(s.observability.host, s.observability.port, "观测面（server.observability_listen）");
  if (!obs) return fail(obs.error());
  impl_->control = std::make_unique<detail::ControlPlane>(o.config, &impl_->shared);
  if (auto r = impl_->control->listen(*admin, *obs); !r) return r;

  for (auto& w : impl_->workers) w->start();
  impl_->control->start();
  impl_->started = true;
  return {};
}

void Server::stop() {
  if (!impl_) return;
  impl_->shared.stopping.store(true, std::memory_order_relaxed);  // /ready 立即返回 503
  for (auto& w : impl_->workers) w->stop();
  if (impl_->control) impl_->control->stop();
  impl_->workers.clear();  // 关闭所有连接（析构时销毁未完成的 Session）
  impl_->control.reset();
}

std::uint16_t Server::data_port() const {
  return impl_->workers.empty() ? 0 : impl_->workers.front()->port();
}
std::uint16_t Server::admin_port() const {
  return impl_->control ? impl_->control->admin_port() : 0;
}
std::uint16_t Server::observability_port() const {
  return impl_->control ? impl_->control->observability_port() : 0;
}
unsigned Server::workers() const { return static_cast<unsigned>(impl_->workers.size()); }

void pregrow_fd_table() {
  rlimit rl{};
  if (getrlimit(RLIMIT_NOFILE, &rl) != 0 || rl.rlim_cur < 2) return;
  // 上限超过 2^20 时只扩到 2^20（每个 fd 槽约 8 字节）。用 F_DUPFD 而不是 dup2：dup2
  // 会关闭已打开的目标 fd
  const int top = static_cast<int>(std::min<rlim_t>(rl.rlim_cur, rlim_t{1} << 20U)) - 1;
  const int fd = fcntl(STDERR_FILENO, F_DUPFD_CLOEXEC, top);
  if (fd >= 0) close(fd);
}

}  // namespace llmgw::server
