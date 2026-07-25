#ifndef CONFIG_H
#define CONFIG_H

#include <cstddef>
#include <string>
#include <vector>

// One saved tvheadend server.
struct ServerProfile {
  std::string name; // display name; falls back to host when empty
  std::string host;
  std::string port = "9982";
  std::string user;
  std::string pass;

  // What the connect screen shows under the name.
  std::string Label() const;
  std::string Detail() const;
  int PortNumber() const;
  bool Valid() const { return !host.empty(); }
};

// The list of saved servers, persisted to tvhp.cfg next to the executable.
//
// File format (the old single-server format, a flat list of host=/port=/
// user=/pass= lines, is still read and silently migrated on the next save):
//
//   last=1
//   [server]
//   name=Living room
//   host=192.168.1.10
//   port=9982
//   user=tv
//   pass=secret
//   [server]
//   ...
//
// Passwords are stored in the clear, exactly as the previous single-server
// config did. Anyone with access to the console's filesystem can read them.
class ServerConfig {
public:
  // Reads the config file, then applies the TVH_HOST / TVH_PORT / TVH_USER /
  // TVH_PASS environment overrides on top of the last-used entry (adding one
  // when the file is empty), so scripted launches keep working.
  void Load(const std::string& path);
  bool Save(const std::string& path) const;

  const std::vector<ServerProfile>& Servers() const { return servers_; }
  size_t Count() const { return servers_.size(); }
  bool Empty() const { return servers_.empty(); }

  // Index of the server used for the last successful connection, clamped to
  // the list. Returns 0 for an empty list.
  int LastUsed() const;
  void SetLastUsed(int index);

  const ServerProfile* At(int index) const;

  // Adds a profile and returns its index, or updates the one at 'index'.
  int Add(const ServerProfile& profile);
  void Update(int index, const ServerProfile& profile);
  void Remove(int index);

private:
  std::vector<ServerProfile> servers_;
  int last_used_ = 0;
};

#endif
