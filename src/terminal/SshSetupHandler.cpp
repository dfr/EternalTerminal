#include "SshSetupHandler.hpp"

#include <sys/wait.h>

namespace et {
string genRandom(int len) {
  static const char alphanum[] =
      "0123456789"
      "ABCDEFGHIJKLMNOPQRSTUVWXYZ"
      "abcdefghijklmnopqrstuvwxyz";
  string s(len, '\0');

  for (int i = 0; i < len; ++i) {
    s[i] = alphanum[rand() % (sizeof(alphanum) - 1)];
  }

  return s;
}

string SshSetupHandler::SetupSsh(string user, string host, string host_alias,
                                 int port, string jumphost, int jport,
                                 bool kill) {
  string CLIENT_TERM(getenv("TERM"));
  srand(time(NULL));
  string passkey = genRandom(32);
  string id = genRandom(16);
  string SSH_SCRIPT_PREFIX{
      "SERVER_TMP_DIR=${TMPDIR:-${TMP:-${TEMP:-/tmp}}};"
      "TMPFILE=$(mktemp $SERVER_TMP_DIR/et-server.XXXXXXXXXXXX);"
      "PASSKEY=" +
      passkey +
      ";"
      "ID=" +
      id +
      ";"
      "printf \"%s/%s\\n\" \"$ID\" \"$PASSKEY\" > \"${TMPFILE}\";"
      "export TERM=" +
      CLIENT_TERM +
      ";"
      "etserver --idpasskeyfile=\"${TMPFILE}\""};

  // Kill old ET sessions of the user
  if (kill && user != "root") {
    SSH_SCRIPT_PREFIX = "pkill etserver -u " + user + ";" + SSH_SCRIPT_PREFIX;
  }
  string SSH_SCRIPT_DST = SSH_SCRIPT_PREFIX + ";true";

  int link_client[2];
  char buf_client[4096];
  if (pipe(link_client) == -1) {
    LOG(FATAL) << "pipe";
    exit(1);
  }

  pid_t pid = fork();
  string SSH_USER_PREFIX = "";
  if (!user.empty()) {
    SSH_USER_PREFIX += user + "@";
  }
  if (!pid) {
    // start etserver daemon on dst.
    dup2(link_client[1], 1);
    close(link_client[0]);
    close(link_client[1]);
    // run the command in interactive mode
    SSH_SCRIPT_DST = "$SHELL -lc \'" + SSH_SCRIPT_DST + "\'";
    if (!jumphost.empty()) {
      execlp("ssh", "ssh", "-J", (SSH_USER_PREFIX + jumphost).c_str(),
             (SSH_USER_PREFIX + host_alias).c_str(), (SSH_SCRIPT_DST).c_str(),
             NULL);
    } else {
      execlp("ssh", "ssh", (SSH_USER_PREFIX + host_alias).c_str(),
             SSH_SCRIPT_DST.c_str(), NULL);
    }

    LOG(INFO) << "execl error";
    exit(1);
  } else if (pid < 0) {
    LOG(INFO) << "Failed to fork";
    exit(1);
  } else {
    close(link_client[1]);
    wait(NULL);
    int nbytes = read(link_client[0], buf_client, sizeof(buf_client));
    try {
      if (nbytes <= 0 || split(string(buf_client), ':').size() != 2 ||
          split(string(buf_client), ':')[0] != "IDPASSKEY") {
        cout << "Error:  The Eternal Terminal daemon is not running.  "
                "Please (re)start the et daemon on the server."
             << endl;
        exit(1);
      }
      auto idpasskey = split(string(buf_client), ':')[1];
      idpasskey = idpasskey.substr(0, 16 + 1 + 32);
      auto idpasskey_splited = split(idpasskey, '/');
      string returned_id = idpasskey_splited[0];
      string returned_passkey = idpasskey_splited[1];
      if (returned_id == id && returned_passkey == passkey) {
        LOG(INFO) << "etserver started";
      } else {
        LOG(FATAL) << "client/server idpasskey doesn't match: " << id
                   << " != " << returned_id << " or " << passkey
                   << " != " << returned_passkey;
      }
    } catch (const runtime_error &err) {
      cout << "Error initializing connection" << err.what() << endl;
    }
    // start jumpclient daemon on jumphost.
    if (!jumphost.empty()) {
      /* If jumphost is set, we need to pass dst host and port to jumphost
       * and connect to jumphost here */
      int link_jump[2];
      char buf_jump[4096];
      if (pipe(link_jump) == -1) {
        LOG(FATAL) << "pipe";
        exit(1);
      }
      pid_t pid_jump = fork();
      if (pid_jump < 0) {
        LOG(FATAL) << "Failed to fork";
        exit(1);
      } else if (pid_jump == 0) {
        dup2(link_jump[1], 1);
        close(link_jump[0]);
        close(link_jump[1]);
        string cmdoptions = "";
        cmdoptions +=
            " --jump --dsthost=" + host + " --dstport=" + to_string(port);
        string SSH_SCRIPT_JUMP = SSH_SCRIPT_PREFIX + cmdoptions + ";true";
        // start command in interactive mode
        SSH_SCRIPT_JUMP = "$SHELL -lc \'" + SSH_SCRIPT_JUMP + "\'";
        execlp("ssh", "ssh", jumphost.c_str(), SSH_SCRIPT_JUMP.c_str(), NULL);
      } else {
        close(link_jump[1]);
        wait(NULL);
        int nbytes = read(link_jump[0], buf_jump, sizeof(buf_jump));
        if (nbytes <= 0) {
          LOG(FATAL) << "etserver jumpclient failed to start";
          exit(1);
        }
        try {
          auto idpasskey = split(string(buf_jump), ':')[1];
          idpasskey.erase(idpasskey.find_last_not_of(" \n\r\t") + 1);
          idpasskey = idpasskey.substr(0, 16 + 1 + 32);
          auto idpasskey_splited = split(idpasskey, '/');
          string returned_id = idpasskey_splited[0];
          string returned_passkey = idpasskey_splited[1];
          if (returned_id == id && returned_passkey == passkey) {
            LOG(INFO) << "jump client started.";
          } else {
            LOG(INFO) << "ID " << id;
            LOG(INFO) << "Received ID " << returned_id;
            LOG(INFO) << "PASSKEY " << passkey;
            LOG(INFO) << "Received PASSKEY " << returned_passkey;
            LOG(INFO) << "client/server idpasskey doesn't match";
            exit(1);
          }
        } catch (const runtime_error &err) {
          cout << "Error initializing connection" << err.what() << endl;
        }
      }
    }
  }
  return id + "/" + passkey;
}
}  // namespace et