#include <algorithm>
#include <cstdlib>
#include <fstream>

#include "config.h"

namespace {

std::string Trim(const std::string& s)
{
  const size_t begin = s.find_first_not_of(" \t\r\n");
  if (begin == std::string::npos)
    return {};
  const size_t end = s.find_last_not_of(" \t\r\n");
  return s.substr(begin, end - begin + 1);
}

} // namespace

std::string ServerProfile::Label() const
{
  if (!name.empty())
    return name;
  if (!host.empty())
    return host;
  return "New server";
}

std::string ServerProfile::Detail() const
{
  std::string out = host.empty() ? std::string("not configured") : host;
  if (!port.empty() && port != "9982")
    out += ":" + port;
  if (!user.empty())
    out += "  -  " + user;
  return out;
}

int ServerProfile::PortNumber() const
{
  const int p = std::atoi(port.c_str());
  return (p > 0 && p <= 65535) ? p : 9982;
}

void ServerConfig::Load(const std::string& path)
{
  servers_.clear();
  last_used_ = 0;

  std::ifstream f(path);
  std::string line;
  ServerProfile current;
  bool in_section = false;

  auto flush = [&]() {
    if (in_section && current.Valid())
      servers_.push_back(current);
    current = ServerProfile();
    in_section = false;
  };

  while (std::getline(f, line))
    {
      line = Trim(line);
      if (line.empty() || line[0] == '#')
	continue;

      if (line == "[server]")
	{
	  flush();
	  in_section = true;
	  continue;
	}

      const size_t eq = line.find('=');
      if (eq == std::string::npos)
	continue;
      const std::string key = Trim(line.substr(0, eq));
      const std::string val = Trim(line.substr(eq + 1));

      if (key == "last")
	{
	  last_used_ = std::atoi(val.c_str());
	  continue;
	}

      // Keys outside any [server] block are the old single-server format;
      // treat them as the first (implicit) profile.
      if (!in_section)
	in_section = true;

      if (key == "name")
	current.name = val;
      else if (key == "host")
	current.host = val;
      else if (key == "port")
	current.port = val;
      else if (key == "user")
	current.user = val;
      else if (key == "pass")
	current.pass = val;
    }
  flush();

  // Environment overrides apply to the entry that will be preselected, which
  // keeps TVH_HOST=... ./tvhp working the way it always has.
  const char* env_host = std::getenv("TVH_HOST");
  const char* env_port = std::getenv("TVH_PORT");
  const char* env_user = std::getenv("TVH_USER");
  const char* env_pass = std::getenv("TVH_PASS");
  if (env_host || env_port || env_user || env_pass)
    {
      if (servers_.empty())
	{
	  servers_.push_back(ServerProfile());
	  last_used_ = 0;
	}
      ServerProfile& p = servers_[LastUsed()];
      if (env_host) p.host = env_host;
      if (env_port) p.port = env_port;
      if (env_user) p.user = env_user;
      if (env_pass) p.pass = env_pass;
    }

  last_used_ = LastUsed();
}

bool ServerConfig::Save(const std::string& path) const
{
  std::ofstream f(path, std::ios::trunc);
  if (!f)
    return false;

  f << "# tvhp saved servers. Passwords are stored in the clear.\n";
  f << "last=" << last_used_ << "\n";
  for (const ServerProfile& p : servers_)
    {
      f << "[server]\n";
      if (!p.name.empty())
	f << "name=" << p.name << "\n";
      f << "host=" << p.host << "\n"
	<< "port=" << p.port << "\n"
	<< "user=" << p.user << "\n"
	<< "pass=" << p.pass << "\n";
    }
  return true;
}

int ServerConfig::LastUsed() const
{
  if (servers_.empty())
    return 0;
  return std::clamp(last_used_, 0, (int)servers_.size() - 1);
}

void ServerConfig::SetLastUsed(int index)
{
  last_used_ = index;
}

const ServerProfile* ServerConfig::At(int index) const
{
  if (index < 0 || index >= (int)servers_.size())
    return nullptr;
  return &servers_[index];
}

int ServerConfig::Add(const ServerProfile& profile)
{
  servers_.push_back(profile);
  return (int)servers_.size() - 1;
}

void ServerConfig::Update(int index, const ServerProfile& profile)
{
  if (index >= 0 && index < (int)servers_.size())
    servers_[index] = profile;
}

void ServerConfig::Remove(int index)
{
  if (index < 0 || index >= (int)servers_.size())
    return;
  servers_.erase(servers_.begin() + index);
  if (last_used_ >= (int)servers_.size())
    last_used_ = std::max(0, (int)servers_.size() - 1);
}
