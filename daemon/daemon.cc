// Copyright (c) 2012, Robert Escriva
// All rights reserved.
//
// Redistribution and use in source and binary forms, with or without
// modification, are permitted provided that the following conditions are met:
//
//     * Redistributions of source code must retain the above copyright notice,
//       this list of conditions and the following disclaimer.
//     * Redistributions in binary form must reproduce the above copyright
//       notice, this list of conditions and the following disclaimer in the
//       documentation and/or other materials provided with the distribution.
//     * Neither the name of Replicant nor the names of its contributors may be
//       used to endorse or promote products derived from this software without
//       specific prior written permission.
//
// THIS SOFTWARE IS PROVIDED BY THE COPYRIGHT HOLDERS AND CONTRIBUTORS "AS IS"
// AND ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT LIMITED TO, THE
// IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS FOR A PARTICULAR PURPOSE
// ARE DISCLAIMED. IN NO EVENT SHALL THE COPYRIGHT OWNER OR CONTRIBUTORS BE
// LIABLE FOR ANY DIRECT, INDIRECT, INCIDENTAL, SPECIAL, EXEMPLARY, OR
// CONSEQUENTIAL DAMAGES (INCLUDING, BUT NOT LIMITED TO, PROCUREMENT OF
// SUBSTITUTE GOODS OR SERVICES; LOSS OF USE, DATA, OR PROFITS; OR BUSINESS
// INTERRUPTION) HOWEVER CAUSED AND ON ANY THEORY OF LIABILITY, WHETHER IN
// CONTRACT, STRICT LIABILITY, OR TORT (INCLUDING NEGLIGENCE OR OTHERWISE)
// ARISING IN ANY WAY OUT OF THE USE OF THIS SOFTWARE, EVEN IF ADVISED OF THE
// POSSIBILITY OF SUCH DAMAGE.

#define __STDC_LIMIT_MACROS

#ifdef HAVE_CONFIG_H
#include "config.h"
#endif

// C
#include <cmath>

// POSIX
#include <dlfcn.h>
#include <signal.h>
#include <sys/types.h>
#include <sys/stat.h>
#include <unistd.h>

// STL
#include <algorithm>

// Google Log
#include <glog/logging.h>
#include <glog/raw_logging.h>

// po6
#include <po6/pathname.h>

// e
#include <e/compat.h>
#include <e/endian.h>
#include <e/envconfig.h>
#include <e/error.h>
#include <e/strescape.h>
#include <e/time.h>

// BusyBee
#include <busybee_constants.h>
#include <busybee_mta.h>
#include <busybee_single.h>

// Replicant
#include "common/bootstrap.h"
#include "common/macros.h"
#include "common/network_msgtype.h"
#include "common/packing.h"
#include "common/special_clients.h"
#include "common/special_objects.h"
#include "daemon/daemon.h"
#include "daemon/heal_next.h"
#include "daemon/request_response.h"

using replicant::daemon;

#define CHECK_UNPACK(MSGTYPE, UNPACKER) \
    do \
    { \
        if (UNPACKER.error()) \
        { \
            replicant_network_msgtype CONCAT(_anon, __LINE__)(REPLNET_ ## MSGTYPE); \
            LOG(WARNING) << "received corrupt \"" \
                         << CONCAT(_anon, __LINE__) << "\" message"; \
            return; \
        } \
    } while (0)

static bool s_continue = true;

static void
exit_on_signal(int /*signum*/)
{
    RAW_LOG(ERROR, "signal received; triggering exit");
    s_continue = false;
}

static uint64_t
monotonic_time()
{
    return e::time();
}

// round x up to a multiple of y
static uint64_t
next_interval(uint64_t x, uint64_t y)
{
    uint64_t z = ((x + y) / y) * y;
    assert(x < z);
    return z;
}

struct daemon::deferred_command
{
    deferred_command() : object(), client(), has_nonce(), nonce(), data() {}
    deferred_command(uint64_t o, uint64_t c,
                     e::compat::shared_ptr<e::buffer> d)
        : object(o), client(c), has_nonce(false), nonce(0), data(d) {}
    deferred_command(uint64_t o, uint64_t c, uint64_t n,
                     e::compat::shared_ptr<e::buffer> d)
        : object(o), client(c), has_nonce(true), nonce(n), data(d) {}
    deferred_command(const deferred_command& other)
        : object(other.object)
        , client(other.client)
        , has_nonce(other.has_nonce)
        , nonce(other.nonce)
        , data(other.data)
    {
    }
    ~deferred_command() throw () {}
    deferred_command& operator = (const deferred_command& rhs)
    {
        if (this != &rhs)
        {
            object = rhs.object;
            client = rhs.client;
            has_nonce = rhs.has_nonce;
            nonce = rhs.nonce;
            data = rhs.data;
        }

        return *this;
    }

    uint64_t object;
    uint64_t client;
    bool has_nonce; // else it's the slot
    uint64_t nonce;
    e::compat::shared_ptr<e::buffer> data;
};

daemon :: ~daemon() throw ()
{
    m_gc.deregister_thread(&m_gc_ts);
}

daemon :: daemon()
    : m_s()
    , m_gc()
    , m_gc_ts()
    , m_busybee_mapper()
    , m_busybee()
    , m_us()
    , m_have_bootstrapped(false)
    , m_maintain_count(0)
    , m_bootstrap()
    , m_bootstrap_thread(po6::threads::make_thread_wrapper(&daemon::background_bootstrap, this))
    , m_bootstrap_mtx()
    , m_bootstrap_cond(&m_bootstrap_mtx)
    , m_config_manager()
    , m_object_manager(&m_gc)
    , m_failure_manager()
    , m_client_manager()
    , m_periodic_mtx()
    , m_periodic()
    , m_send_mtx()
    , m_deferred_mtx()
    , m_deferred(new std::queue<deferred_command>())
    , m_temporary_servers()
    , m_heal_token(0)
    , m_heal_next()
    , m_stable_version(0)
    , m_disrupted_backoff()
    , m_disrupted_retry_scheduled(false)
    , m_fs()
{
    m_periodic_mtx.lock();
    m_periodic.empty();
    m_periodic_mtx.unlock();
    m_deferred_mtx.lock();
    m_deferred->empty();
    m_deferred_mtx.unlock();
    m_object_manager.set_callback(this, &daemon::record_execution,
                                        &daemon::send_notify,
                                        &daemon::handle_snapshot,
                                        &daemon::issue_alarm,
                                        &daemon::issue_suspect_callback);
    trip_periodic(0, &daemon::periodic_describe_slots);
    trip_periodic(0, &daemon::periodic_exchange);
    trip_periodic(0, &daemon::periodic_suspect_clients);
    trip_periodic(0, &daemon::periodic_disconnect_clients);
    trip_periodic(0, &daemon::periodic_alarm);
    m_gc.register_thread(&m_gc_ts);
}

static bool
install_signal_handler(int signum)
{
    struct sigaction handle;
    handle.sa_handler = exit_on_signal;
    sigfillset(&handle.sa_mask);
    handle.sa_flags = SA_RESTART;
    return sigaction(signum, &handle, NULL) >= 0;
}

int
daemon :: run(bool daemonize,
              po6::pathname data,
              po6::pathname log,
              po6::pathname pidfile,
              bool has_pidfile,
              bool set_bind_to,
              po6::net::location bind_to,
              bool set_existing,
              const std::vector<po6::net::hostname>& existing,
              const char* init_obj,
              const char* init_lib,
              const char* init_str,
              const char* init_rst)
{
    if (!install_signal_handler(SIGHUP))
    {
        std::cerr << "could not install SIGHUP handler; exiting" << std::endl;
        return EXIT_FAILURE;
    }

    if (!install_signal_handler(SIGINT))
    {
        std::cerr << "could not install SIGINT handler; exiting" << std::endl;
        return EXIT_FAILURE;
    }

    if (!install_signal_handler(SIGTERM))
    {
        std::cerr << "could not install SIGTERM handler; exiting" << std::endl;
        return EXIT_FAILURE;
    }

    sigset_t ss;

    if (sigfillset(&ss) < 0)
    {
        PLOG(ERROR) << "could not block signals";
        return EXIT_FAILURE;
    }

    int err = pthread_sigmask(SIG_BLOCK, &ss, NULL);

    if (err < 0)
    {
        errno = err;
        PLOG(ERROR) << "could not block signals";
        return EXIT_FAILURE;
    }

    google::LogToStderr();

    if (daemonize)
    {
        struct stat x;

        if (lstat(log.get(), &x) < 0 || !S_ISDIR(x.st_mode))
        {
            LOG(ERROR) << "cannot fork off to the background because "
                       << log.get() << " does not exist or is not writable";
            return EXIT_FAILURE;
        }

        if (!has_pidfile)
        {
            LOG(INFO) << "forking off to the background";
            LOG(INFO) << "you can find the log at " << log.get() << "/replicant-daemon-YYYYMMDD-HHMMSS.sssss";
            LOG(INFO) << "provide \"--foreground\" on the command-line if you want to run in the foreground";
        }

        google::SetLogSymlink(google::INFO, "");
        google::SetLogSymlink(google::WARNING, "");
        google::SetLogSymlink(google::ERROR, "");
        google::SetLogSymlink(google::FATAL, "");
        log = po6::join(log, "replicant-daemon-");
        google::SetLogDestination(google::INFO, log.get());

        if (::daemon(1, 0) < 0)
        {
            PLOG(ERROR) << "could not daemonize";
            return EXIT_FAILURE;
        }

        if (has_pidfile)
        {
            char buf[21];
            ssize_t buf_sz = sprintf(buf, "%d\n", getpid());
            assert(buf_sz < static_cast<ssize_t>(sizeof(buf)));
            po6::io::fd pid(open(pidfile.get(), O_WRONLY|O_CREAT|O_TRUNC, S_IRUSR|S_IWUSR));

            if (pid.get() < 0 || pid.xwrite(buf, buf_sz) != buf_sz)
            {
                PLOG(ERROR) << "could not create pidfile " << pidfile.get();
                return EXIT_FAILURE;
            }
        }
    }
    else
    {
        LOG(INFO) << "running in the foreground";
        LOG(INFO) << "no log will be generated; instead, the log messages will print to the terminal";
        LOG(INFO) << "provide \"--daemon\" on the command-line if you want to run in the background";
    }

    bool restored = false;
    chain_node restored_us;
    configuration_manager restored_config_manager;

    if (!m_fs.open(data, &restored, &restored_us, &restored_config_manager))
    {
        return EXIT_FAILURE;
    }

    if (strlen(data.dirname().get()))
    {
        if (chdir(data.dirname().get()) < 0)
        {
            PLOG(ERROR) << "could not change cwd to data directory";
            return EXIT_FAILURE;
        }
    }

    m_us.address = bind_to;
    bool init = false;

    // case 1:  start a new cluster
    if (!restored && !set_existing)
    {
        uint64_t cluster_id;
        uint64_t this_token;

        if (!generate_token(&m_us.token) ||
            !generate_token(&cluster_id) ||
            !generate_token(&this_token))
        {
            PLOG(ERROR) << "could not read random tokens from /dev/urandom";
            m_fs.wipe();
            return EXIT_FAILURE;
        }

        configuration initial(cluster_id, 0, this_token, 1, m_us);
        m_config_manager.reset(initial);
        m_fs.inform_configuration(initial);
        LOG(INFO) << "started new cluster from command-line arguments: " << initial;
        init = init_obj && init_lib;
    }
    // case 2: joining a new cluster
    else if (!restored && set_existing)
    {
        LOG(INFO) << "starting new daemon from command-line arguments using "
                  << bootstrap_hosts_to_string(&existing[0], existing.size())
                  << " as our bootstrap node";
        configuration initial;

        switch (bootstrap(&existing[0], existing.size(), &initial))
        {
            case replicant::BOOTSTRAP_SUCCESS:
                break;
            case replicant::BOOTSTRAP_SEE_ERRNO:
                LOG(ERROR) << "cannot connect to cluster: " << e::error::strerror(errno);
                return EXIT_FAILURE;
            case replicant::BOOTSTRAP_COMM_FAIL:
                LOG(ERROR) << "cannot connect to cluster: internal error: " << e::error::strerror(errno);
                return EXIT_FAILURE;
            case replicant::BOOTSTRAP_TIMEOUT:
                LOG(ERROR) << "cannot connect to cluster: operation timed out";
                return EXIT_FAILURE;
            case replicant::BOOTSTRAP_CORRUPT_INFORM:
                LOG(ERROR) << "cannot connect to cluster: server sent a corrupt INFORM message";
                return EXIT_FAILURE;
            case replicant::BOOTSTRAP_NOT_CLUSTER_MEMBER:
                LOG(ERROR) << "cannot connect to cluster: server is not a member of the cluster";
                return EXIT_FAILURE;
            case replicant::BOOTSTRAP_GARBAGE:
            default:
                LOG(ERROR) << "cannot connect to cluster: bootstrap failed";
                return EXIT_FAILURE;
        }

        LOG(INFO) << "successfully bootstrapped with " << initial;
        const chain_node* head = initial.head();

        if (!generate_token(&m_us.token))
        {
            PLOG(ERROR) << "could not read server_id from /dev/urandom";
            m_fs.wipe();
            return EXIT_FAILURE;
        }

        if (initial.has_token(m_us.token))
        {
            LOG(ERROR) << "by some freak coincidence, we've picked the same random number that someone else did previously";
            LOG(ERROR) << "since we are picking 64-bit numbers, this is extremely unlikely";
            LOG(ERROR) << "if you re-launch the daemon, we'll try picking a different number, but you will want to check for errors in your environment";
            m_fs.wipe();
            return EXIT_FAILURE;
        }

        // XXX check for address conflict

        LOG(INFO) << "registering with the head of the configuration: " << *head;
        std::auto_ptr<e::buffer> request;
        request.reset(e::buffer::create(BUSYBEE_HEADER_SIZE
                                       + pack_size(REPLNET_SERVER_REGISTER)
                                       + pack_size(m_us)));
        request->pack_at(BUSYBEE_HEADER_SIZE) << REPLNET_SERVER_REGISTER << m_us;
        std::auto_ptr<e::buffer> response;

        if (!request_response(head->address, 5000, request, "registering with ", &response))
        {
            m_fs.wipe();
            return EXIT_FAILURE;
        }

        replicant_network_msgtype mt = REPLNET_NOP;
        e::unpacker up = response->unpack_from(BUSYBEE_HEADER_SIZE);
        up = up >> mt;

        if (up.error())
        {
            LOG(ERROR) << "received corrupt response to registration request";
            m_fs.wipe();
            return EXIT_FAILURE;
        }

        if (mt == REPLNET_SERVER_REGISTER_FAILED)
        {
            LOG(ERROR) << "failed to register with the cluster";
            LOG(ERROR) << "check to make sure that no one else is using our token or address";
            LOG(ERROR) << "us=" << m_us;
            m_fs.wipe();
            return EXIT_FAILURE;
        }

        up = up >> initial;

        if (up.error() ||
            mt != REPLNET_INFORM ||
            !initial.validate())
        {
            LOG(ERROR) << "received invalid INFORM message from " << *head;
            m_fs.wipe();
            return EXIT_FAILURE;
        }

        m_config_manager.reset(initial);
        m_fs.inform_configuration(initial);
        LOG(INFO) << "started new cluster from command-line arguments: " << initial;
        LOG(INFO) << "joined existing cluster as " << m_us << ": " << initial;
    }
    else
    {
        LOG(INFO) << "restoring previous instance: " << restored_us.token;
        m_us.token = restored_us.token;

        if (!set_bind_to)
        {
            m_us.address = restored_us.address;
        }
    }

    if (!init && init_rst)
    {
        LOG(INFO) << "asked to restore from \"" << e::strescape(init_rst) << "\" "
                  << "but we are not initializing a new cluster";
        LOG(INFO) << "the restore operations only have an effect when "
                  << "starting a fresh cluster";
        LOG(INFO) << "this likely means you'll want to start with a new data-dir "
                  << "and omit any options for connecting to an existing cluster";
        return EXIT_FAILURE;
    }

    m_busybee.reset(new busybee_mta(&m_gc, &m_busybee_mapper, m_us.address, m_us.token, 0/*we don't use pause/unpause*/));
    m_busybee->set_timeout(1);

    if (!restored)
    {
        m_fs.save(m_us);
    }

    if (restored)
    {
        m_config_manager = restored_config_manager;
    }

    for (size_t slot = 1; slot < m_fs.next_slot_to_ack(); ++slot)
    {
        uint64_t object;
        uint64_t client;
        uint64_t nonce;
        e::slice dat;
        std::string backing;

        if (!m_fs.get_slot(slot, &object, &client, &nonce, &dat, &backing))
        {
            LOG(ERROR) << "gap in the history; missing slot " << slot;
            return EXIT_FAILURE;
        }

        if (object == OBJECT_OBJ_NEW ||
            object == OBJECT_OBJ_DEL ||
            object == OBJECT_OBJ_SNAPSHOT ||
            object == OBJECT_OBJ_RESTORE ||
            !IS_SPECIAL_OBJECT(object))
        {
            m_object_manager.enqueue(slot, object, client, nonce, dat);
            m_object_manager.throttle(object, 16);
        }
        else if (object == OBJECT_CLI_REG)
        {
            m_client_manager.register_client(client);
            m_client_manager.proof_of_life(client, monotonic_time());
        }
        else if (object == OBJECT_CLI_DIE)
        {
            m_client_manager.deregister_client(client);
        }
    }

    m_object_manager.enable_logging();

    if (init)
    {
        assert(init_obj);
        assert(init_lib);
        assert(m_fs.next_slot_to_issue() == 1);
        assert(m_fs.next_slot_to_ack() == 1);
        std::vector<char> lib(sizeof(uint64_t) + sizeof(uint32_t));

        // Encode the object name
        assert(strlen(init_obj) <= sizeof(uint64_t));
        memset(&lib[0], 0, sizeof(lib.size()));
        memmove(&lib[0], init_obj, strlen(init_obj));
        uint64_t obj = 0;
        e::unpack64be(&lib[0], &obj);

        // Read the library
        char buf[4096];
        po6::io::fd fd(open(init_lib, O_RDONLY));

        if (fd.get() < 0)
        {
            PLOG(ERROR) << "could not open library";
            return EXIT_FAILURE;
        }

        ssize_t amt = 0;

        while ((amt = fd.xread(buf, 4096)) > 0)
        {
            size_t tmp = lib.size();
            lib.resize(tmp + amt);
            memmove(&lib[tmp], buf, amt);
        }

        if (amt < 0)
        {
            PLOG(ERROR) << "could not read library";
            return EXIT_FAILURE;
        }

        // if this is a restore
        if (init_rst)
        {
            size_t offset = lib.size();
            lib.resize(offset + sizeof(uint32_t));
            fd = open(init_rst, O_RDONLY);

            if (fd.get() < 0)
            {
                PLOG(ERROR) << "could not open restore file";
                return EXIT_FAILURE;
            }

            while ((amt = fd.xread(buf, 4096)) > 0)
            {
                size_t tmp = lib.size();
                lib.resize(tmp + amt);
                memmove(&lib[tmp], buf, amt);
            }

            if (amt < 0)
            {
                PLOG(ERROR) << "could not read restore file";
                return EXIT_FAILURE;
            }

            uint32_t lib_sz = offset - sizeof(uint64_t) - sizeof(uint32_t);
            uint32_t rst_sz = lib.size() - offset - sizeof(uint32_t);
            e::pack32be(lib_sz, &lib[sizeof(uint64_t)]);
            e::pack32be(rst_sz, &lib[offset]);
            e::slice cmd_slice(&lib[0], lib.size());
            issue_command(1, OBJECT_OBJ_RESTORE, 0, 1, cmd_slice);
            LOG(INFO) << "restoring " << init_obj << " from \"" << e::strescape(init_rst) << "\"";
        }
        // else this is an initialization
        else
        {
            e::pack32be(lib.size() - sizeof(uint64_t) - sizeof(uint32_t),
                        &lib[sizeof(uint64_t)]);
            e::slice cmd_slice(&lib[0], lib.size());
            issue_command(1, OBJECT_OBJ_NEW, 0, 1, cmd_slice);
            LOG(INFO) << "initializing " << init_obj << " with \"" << e::strescape(init_lib) << "\"";

            if (init_str)
            {
                std::vector<char> init_buf(5 + strlen(init_str) + 1);
                memmove(&init_buf[0], "init\x00", 5);
                memmove(&init_buf[5], init_str, strlen(init_str) + 1);
                e::slice init_slice(&init_buf[0], init_buf.size());
                issue_command(2, obj, 0, 2, init_slice);
            }
        }
    }

    m_fs.warm_cache();
    LOG(INFO) << "resuming normal operation";
    post_reconfiguration_hooks();
    m_bootstrap_mtx.lock();
    m_have_bootstrapped = false;
    m_bootstrap_mtx.unlock();
    m_bootstrap = existing;
    m_bootstrap_thread.start();

    replicant::connection conn;
    std::auto_ptr<e::buffer> msg;

    while (recv(&conn, &msg))
    {
        assert(msg.get());
        replicant_network_msgtype mt = REPLNET_NOP;
        e::unpacker up = msg->unpack_from(BUSYBEE_HEADER_SIZE);
        up = up >> mt;

        switch (mt)
        {
            case REPLNET_NOP:
                break;
            case REPLNET_BOOTSTRAP:
                process_bootstrap(conn, msg, up);
                break;
            case REPLNET_INFORM:
                process_inform(conn, msg, up);
                break;
            case REPLNET_SERVER_REGISTER:
                process_server_register(conn, msg, up);
                break;
            case REPLNET_SERVER_REGISTER_FAILED:
                LOG(WARNING) << "dropping \"SERVER_REGISTER_FAILED\" received by server";
                break;
            case REPLNET_SERVER_CHANGE_ADDRESS:
                process_server_change_address(conn, msg, up);
                break;
            case REPLNET_SERVER_IDENTIFY:
                process_server_identify(conn, msg, up);
                break;
            case REPLNET_SERVER_IDENTITY:
                process_server_identity(conn, msg, up);
                break;
            case REPLNET_CONFIG_PROPOSE:
                process_config_propose(conn, msg, up);
                break;
            case REPLNET_CONFIG_ACCEPT:
                process_config_accept(conn, msg, up);
                break;
            case REPLNET_CONFIG_REJECT:
                process_config_reject(conn, msg, up);
                break;
            case REPLNET_CLIENT_REGISTER:
                process_client_register(conn, msg, up);
                break;
            case REPLNET_CLIENT_DISCONNECT:
                process_client_disconnect(conn, msg, up);
                break;
            case REPLNET_CLIENT_TIMEOUT:
                process_client_timeout(conn, msg, up);
                break;
            case REPLNET_CLIENT_UNKNOWN:
                LOG(WARNING) << "dropping \"CLIENT_UNKNOWN\" received by server";
                break;
            case REPLNET_CLIENT_DECEASED:
                LOG(WARNING) << "dropping \"CLIENT_DECEASED\" received by server";
                break;
            case REPLNET_COMMAND_SUBMIT:
                process_command_submit(conn, msg, up);
                break;
            case REPLNET_COMMAND_ISSUE:
                process_command_issue(conn, msg, up);
                break;
            case REPLNET_COMMAND_ACK:
                process_command_ack(conn, msg, up);
                break;
            case REPLNET_COMMAND_RESPONSE:
                LOG(WARNING) << "dropping \"RESPONSE\" received by server";
                break;
            case REPLNET_HEAL_REQ:
                process_heal_req(conn, msg, up);
                break;
            case REPLNET_HEAL_RETRY:
                process_heal_retry(conn, msg, up);
                break;
            case REPLNET_HEAL_RESP:
                process_heal_resp(conn, msg, up);
                break;
            case REPLNET_HEAL_DONE:
                process_heal_done(conn, msg, up);
                break;
            case REPLNET_STABLE:
                process_stable(conn, msg, up);
                break;
            case REPLNET_CONDITION_WAIT:
                process_condition_wait(conn, msg, up);
                break;
            case REPLNET_CONDITION_NOTIFY:
                LOG(WARNING) << "dropping \"CONDITION_NOTIFY\" received by server";
                break;
            case REPLNET_PING:
                process_ping(conn, msg, up);
                break;
            case REPLNET_PONG:
                process_pong(conn, msg, up);
                break;
            default:
                LOG(WARNING) << "unknown message type; here's some hex:  " << msg->hex();
                break;
        }

        sigset_t pending;
        sigemptyset(&pending);

        if (sigpending(&pending) == 0 &&
            (sigismember(&pending, SIGHUP) ||
             sigismember(&pending, SIGINT) ||
             sigismember(&pending, SIGTERM)))
        {
            exit_on_signal(SIGTERM);
        }

        m_gc.quiescent_state(&m_gc_ts);
    }

    m_bootstrap_mtx.lock();
    m_bootstrap_cond.signal();
    s_continue = false;
    m_bootstrap_mtx.unlock();
    m_bootstrap_thread.join();
    LOG(INFO) << "replicant is gracefully shutting down";
    LOG(INFO) << "replicant will now terminate";
    return EXIT_SUCCESS;
}

void
daemon :: process_bootstrap(const replicant::connection& conn,
                            std::auto_ptr<e::buffer>,
                            e::unpacker)
{
    LOG(INFO) << "providing configuration to "
              << conn.token << "/" << conn.addr
              << " as part of the bootstrap process";
    send(conn, create_inform_message());
}

void
daemon :: process_inform(const replicant::connection&,
                         std::auto_ptr<e::buffer>,
                         e::unpacker up)
{
    configuration new_config;
    up = up >> new_config;
    CHECK_UNPACK(INFORM, up);

    if (m_config_manager.latest().cluster() != new_config.cluster())
    {
        LOG(INFO) << "potential cross-cluster conflict between us="
                  << m_config_manager.latest().cluster()
                  << " and them=" << new_config.cluster();
        return;
    }

    m_fs.inform_configuration(new_config);

    if (m_config_manager.stable().version() < new_config.version())
    {
        LOG(INFO) << "informed about configuration "
                  << new_config.version()
                  << " which replaces stable configuration "
                  << m_config_manager.stable().version();

        if (m_config_manager.contains(new_config))
        {
            m_config_manager.advance(new_config);
        }
        else
        {
            m_config_manager.reset(new_config);
        }

        post_reconfiguration_hooks();
    }
}

void
daemon :: process_server_register(const replicant::connection& conn,
                                  std::auto_ptr<e::buffer>,
                                  e::unpacker up)
{
    chain_node sender;
    up = up >> sender;
    CHECK_UNPACK(SERVER_REGISTER, up);
    LOG(INFO) << "received \"SERVER_REGISTER\" message from "
              << conn.token << "/" << conn.addr << " as " << sender;

    bool success = true;

    if (success && m_config_manager.any(&configuration::has_token, sender.token))
    {
        LOG(INFO) << "not acting on \"SERVER_REGISTER\" message because "
                  << sender << " is in use already";
        send(sender, create_inform_message());
        success = false;
    }

    if (success && m_config_manager.stable().head()->token != m_us.token)
    {
        LOG(INFO) << "not acting on \"SERVER_REGISTER\" message because we are not the head";
        send(sender, create_inform_message());
        success = false;
    }

    configuration new_config = m_config_manager.latest();
    assert(!new_config.has_token(sender.token));
    assert(!new_config.is_member(sender));

    new_config.bump_version();
    new_config.add_member(sender);

    if (success && new_config.validate())
    {
        LOG(INFO) << "propsing configuration " << new_config.version()
                  << " to integrate " << sender << " as a cluster member";
        m_temporary_servers.insert(std::make_pair(conn.token, new_config.version()));
        propose_config(new_config);
    }
    else
    {
        LOG(ERROR) << "trying to register server, but the config doesn't validate; "
                   << "telling the server that registration failed";
        success = false;
    }

    if (!success)
    {
        std::auto_ptr<e::buffer> msg;
        msg.reset(e::buffer::create(BUSYBEE_HEADER_SIZE + pack_size(REPLNET_SERVER_REGISTER_FAILED)));
        msg->pack_at(BUSYBEE_HEADER_SIZE) << REPLNET_SERVER_REGISTER_FAILED;
        send(conn, msg);
    }
}

void
daemon :: process_server_change_address(const replicant::connection& conn,
                                        std::auto_ptr<e::buffer>,
                                        e::unpacker up)
{
    po6::net::location old_address;
    po6::net::location new_address;
    up = up >> old_address >> new_address;
    CHECK_UNPACK(SERVER_CHANGE_ADDRESS, up);
    const chain_node* head = m_config_manager.stable().head();

    if (head->token != m_us.token)
    {
        LOG(INFO) << "cannot change address on behalf of " << conn.token << " (from "
                  << old_address << " to " << new_address
                  << ") because we are not the head; the server will retry on its own";
        return;
    }

    LOG(INFO) << "proposing new configuration on behalf of " << conn.token
              << " which changes its address from " << old_address << " to "
              << new_address;
    configuration new_config = m_config_manager.latest();
    new_config.change_address(conn.token, new_address);
    new_config.bump_version();
    propose_config(new_config);
}

void
daemon :: process_server_identify(const replicant::connection& conn,
                                  std::auto_ptr<e::buffer>,
                                  e::unpacker)
{
    size_t sz = BUSYBEE_HEADER_SIZE
              + pack_size(REPLNET_SERVER_IDENTITY)
              + pack_size(m_us);
    std::auto_ptr<e::buffer> msg(e::buffer::create(sz));
    msg->pack_at(BUSYBEE_HEADER_SIZE) << REPLNET_SERVER_IDENTITY << m_us;
    send(conn, msg);
}

void
daemon :: process_server_identity(const replicant::connection& conn,
                                  std::auto_ptr<e::buffer>,
                                  e::unpacker up)
{
    chain_node them;
    up = up >> them;
    CHECK_UNPACK(SERVER_IDENTITY, up);

    if (conn.token != them.token)
    {
        m_busybee->drop(conn.token);
    }
}

#define SEND_CONFIG_RESP(NODE, ACTION, ID, TIME, US) \
    do \
    { \
        size_t CONCAT(_sz, __LINE__) = BUSYBEE_HEADER_SIZE \
                                     + pack_size(REPLNET_CONFIG_ ## ACTION) \
                                     + 2 * sizeof(uint64_t) \
                                     + pack_size(US); \
        std::auto_ptr<e::buffer> CONCAT(_msg, __LINE__)( \
                e::buffer::create(CONCAT(_sz, __LINE__))); \
        CONCAT(_msg, __LINE__)->pack_at(BUSYBEE_HEADER_SIZE) \
            << (REPLNET_CONFIG_ ## ACTION) << ID << TIME << US; \
        send(NODE, CONCAT(_msg, __LINE__)); \
    } \
    while(0)

void
daemon :: process_config_propose(const replicant::connection& conn,
                                 std::auto_ptr<e::buffer> msg,
                                 e::unpacker up)
{
    uint64_t proposal_id;
    uint64_t proposal_time;
    chain_node sender;
    std::vector<configuration> config_chain;
    up = up >> proposal_id >> proposal_time >> sender >> config_chain;
    CHECK_UNPACK(CONFIG_PROPOSE, up);
    LOG(INFO) << "received proposal " << proposal_id << ":" << proposal_time << " from server=" << conn.token; // XXX dump config_chain

    if (config_chain.empty())
    {
        LOG(ERROR) << "dropping proposal " << proposal_id << ":" << proposal_time
                   << " because it contains no configurations (file a bug): "
                   << msg->as_slice().hex();
        return;
    }

    if (!conn.matches(sender))
    {
        LOG(ERROR) << "dropping proposal " << proposal_id << ":" << proposal_time
                   << "because the sender (" << conn.token << ") does not match "
                   << "the claimed sender (" << sender.token << "); please file a bug: "
                   << msg->as_slice().hex();
        return;
    }

    if (m_fs.is_rejected_configuration(proposal_id, proposal_time))
    {
        SEND_CONFIG_RESP(sender, REJECT, proposal_id, proposal_time, m_us);
        LOG(INFO) << "proposal " << proposal_id << ":" << proposal_time << " previously rejected; response sent";
        return;
    }

    if (m_fs.is_accepted_configuration(proposal_id, proposal_time))
    {
        SEND_CONFIG_RESP(sender, ACCEPT, proposal_id, proposal_time, m_us);
        LOG(INFO) << "proposal " << proposal_id << ":" << proposal_time << " previously accpted; response sent";
        return;
    }

    if (m_fs.is_proposed_configuration(proposal_id, proposal_time))
    {
        LOG(INFO) << "proposal " << proposal_id << ":" << proposal_time << " previously proposed; waiting to receive a response";
        return;
    }

    // idx_stable should be the index of our stable configuration within
    // config_chain
    size_t idx_stable = 0;

    while (idx_stable < config_chain.size() &&
           config_chain[idx_stable] != m_config_manager.stable())
    {
        ++idx_stable;
    }

    if (idx_stable == config_chain.size() &&
        config_chain[0].cluster() == m_config_manager.stable().cluster() &&
        config_chain[0].version() > m_config_manager.stable().version())
    {
        LOG(INFO) << "proposal " << proposal_id << ":" << proposal_time
                  << " is rooted in a stable configuration (" << config_chain[0].version()
                  << ") that supersedes our own;"
                  << " treating it as an inform message";
        m_fs.inform_configuration(config_chain[0]);
        m_config_manager.reset(config_chain[0]);
        post_reconfiguration_hooks();
        idx_stable = 0;
    }
    else if (idx_stable == config_chain.size())
    {
        LOG(ERROR) << "rejecting proposal " << proposal_id << ":" << proposal_time
                   << " that does not contain and supersede our stable configuration (proposed="
                   << m_config_manager.stable().version() << ","
                   << m_config_manager.latest().version() << "; proposal="
                   << config_chain[0].version() << "," << config_chain[config_chain.size() - 1].version()
                   << ")";
        m_fs.propose_configuration(proposal_id, proposal_time, &config_chain.front(), config_chain.size());
        return reject_proposal(sender, proposal_id, proposal_time);
    }

    configuration* configs = &config_chain.front() + idx_stable;
    size_t configs_sz = config_chain.size() - idx_stable;
    m_fs.propose_configuration(proposal_id, proposal_time, configs, configs_sz);

    // Make sure that we could propose it
    if (!m_config_manager.is_compatible(configs, configs_sz))
    {
        LOG(INFO) << "rejecting proposal " << proposal_id << ":" << proposal_time
                  << " that does not merge with current proposals";
        return reject_proposal(sender, proposal_id, proposal_time);
    }

    for (size_t i = 0; i < configs_sz; ++i)
    {
        // Check that the configs are valid
        if (!configs[i].validate())
        {
            LOG(ERROR) << "rejecting proposal " << proposal_id << ":" << proposal_time
                       << " that contains a corrupt configuration at position " << i;
            return reject_proposal(sender, proposal_id, proposal_time);
        }

        // Check that everything is from the same cluster
        if (i + 1 < configs_sz &&
            configs[i].cluster() != configs[i + 1].cluster())
        {
            LOG(ERROR) << "rejecting proposal " << proposal_id << ":" << proposal_time
                       << " that jumps between clusters at position " << i;
            return reject_proposal(sender, proposal_id, proposal_time);
        }

        // Check the sequential links
        if (i + 1 < configs_sz &&
            (configs[i].version() + 1 != configs[i + 1].version() ||
             configs[i].this_token() != configs[i + 1].prev_token()))
        {
            LOG(ERROR) << "rejecting proposal " << proposal_id << ":" << proposal_time
                       << " that violates the configuration chain invariant at position " << i;
            return reject_proposal(sender, proposal_id, proposal_time);
        }

        // Check that the proposed chain meets the quorum requirement
        for (size_t j = i + 1; j < configs_sz; ++j)
        {
            if (!configs[j].quorum_of(configs[i]))
            {
                // This should never happen, so it's an error
                LOG(ERROR) << "rejecting proposal " << proposal_id << ":" << proposal_time
                           << " that violates the configuration quorum invariant";
                return reject_proposal(sender, proposal_id, proposal_time);
            }
        }

        const configuration* bad;

        if (!m_config_manager.contains_quorum_of_all(configs[i], &bad))
        {
            LOG(ERROR) << "rejecting proposal " << proposal_id << ":" << proposal_time
                       << " that violates the configuration quorum invariant with "
                       << *bad;
            return reject_proposal(sender, proposal_id, proposal_time);
        }
    }

    const chain_node* prev = configs[configs_sz - 1].prev(m_us.token);

    if (!prev || prev->token != sender.token)
    {
        // This should never happen, so it's an error
        LOG(ERROR) << "rejecting proposal " << proposal_id << ":" << proposal_time
                   << " that did not propagate along the chain";
        return reject_proposal(sender, proposal_id, proposal_time);
    }

    // If this proposal introduces new configuration versions
    if (m_config_manager.latest().version() < configs[configs_sz - 1].version())
    {
        m_config_manager.merge(proposal_id, proposal_time, configs, configs_sz);

        if (configs[configs_sz - 1].config_tail()->token == m_us.token)
        {
            LOG(INFO) << "proposal " << proposal_id << ":" << proposal_time << " hit the config_tail; adopting";
            m_config_manager.advance(configs[configs_sz - 1]);
            post_reconfiguration_hooks();
            accept_proposal(sender, proposal_id, proposal_time);
        }
        else
        {
            // We must send the whole config_chain and cannot fall back on
            // what we see with configs.
            size_t sz = BUSYBEE_HEADER_SIZE
                      + pack_size(REPLNET_CONFIG_PROPOSE)
                      + 2 * sizeof(uint64_t)
                      + pack_size(m_us)
                      + pack_size(config_chain);
            msg.reset(e::buffer::create(sz));
            msg->pack_at(BUSYBEE_HEADER_SIZE) << REPLNET_CONFIG_PROPOSE
                                              << proposal_id << proposal_time
                                              << m_us << config_chain;
            const chain_node* next = config_chain.back().next(m_us.token);
            assert(next);
            LOG(INFO) << "forwarding proposal " << proposal_id << ":" << proposal_time << " to " << *next;
            send(*next, msg);
        }
    }
}

void
daemon :: process_config_accept(const replicant::connection& conn,
                                std::auto_ptr<e::buffer> msg,
                                e::unpacker up)
{
    uint64_t proposal_id;
    uint64_t proposal_time;
    chain_node sender;
    up = up >> proposal_id >> proposal_time >> sender;
    CHECK_UNPACK(CONFIG_ACCEPT, up);

    if (!conn.matches(sender))
    {
        LOG(ERROR) << "dropping \"CONFIG_ACCEPT\" for proposal "
                   << proposal_id << ":" << proposal_time
                   << "because the sender (" << conn.token << ") does not match "
                   << "the claimed sender (" << sender.token
                   << "); please file a bug: " << msg->as_slice().hex();
        return;
    }

    if (!m_fs.is_proposed_configuration(proposal_id, proposal_time))
    {
        LOG(ERROR) << "dropping \"CONFIG_ACCEPT\" for proposal "
                   << proposal_id << ":" << proposal_time
                   << " from " << conn.token
                   << " because we never saw the proposal";
        return;
    }

    if (m_fs.is_rejected_configuration(proposal_id, proposal_time))
    {
        LOG(ERROR) << "dropping \"CONFIG_ACCEPT\" for proposal "
                   << proposal_id << ":" << proposal_time
                   << " from " << conn.token << " because we rejected it earlier";
        return;
    }

    if (m_fs.is_accepted_configuration(proposal_id, proposal_time))
    {
        // This is a duplicate accept, so we can drop it
        return;
    }

    configuration new_config;

    if (!m_config_manager.get_proposal(proposal_id, proposal_time, &new_config))
    {
        // This proposal was made obsolete by an "INFORM" message
        return;
    }

    const chain_node* next = new_config.next(m_us.token);

    if (!next || next->token != sender.token)
    {
        LOG(ERROR) << "dropping \"CONFIG_ACCEPT\" message that comes from the wrong place"
                   << " " << sender << " instead of " << *next;
        return;
    }

    LOG(INFO) << "accepting proposal " << proposal_id << ":" << proposal_time;
    m_fs.accept_configuration(proposal_id, proposal_time);
    m_config_manager.advance(new_config);
    post_reconfiguration_hooks();
    const chain_node* prev = new_config.prev(m_us.token);

    if (prev)
    {
        SEND_CONFIG_RESP(*prev, ACCEPT, proposal_id, proposal_time, m_us);
    }
}

void
daemon :: process_config_reject(const replicant::connection& conn,
                                std::auto_ptr<e::buffer> msg,
                                e::unpacker up)
{
    uint64_t proposal_id;
    uint64_t proposal_time;
    chain_node sender;
    up = up >> proposal_id >> proposal_time >> sender;
    CHECK_UNPACK(CONFIG_REJECT, up);

    if (!conn.matches(sender))
    {
        LOG(ERROR) << "dropping \"CONFIG_REJECT\" for proposal "
                   << proposal_id << ":" << proposal_time
                   << "because the sender (" << conn.token << ") does not match "
                   << "the claimed sender (" << sender.token
                   << "); please file a bug: " << msg->as_slice().hex();
        return;
    }

    if (!m_fs.is_proposed_configuration(proposal_id, proposal_time))
    {
        LOG(ERROR) << "dropping \"CONFIG_REJECT\" for proposal "
                   << proposal_id << ":" << proposal_time
                   << " from " << conn.token
                   << " because we never saw the proposal";
        return;
    }

    if (m_fs.is_accepted_configuration(proposal_id, proposal_time))
    {
        LOG(ERROR) << "dropping \"CONFIG_REJECT\" for proposal "
                   << proposal_id << ":" << proposal_time
                   << " from " << conn.token << " because we accepted it earlier";
        return;
    }

    if (m_fs.is_rejected_configuration(proposal_id, proposal_time))
    {
        // This is a duplicate reject, so we can drop it
        return;
    }

    configuration new_config;

    if (!m_config_manager.get_proposal(proposal_id, proposal_time, &new_config))
    {
        // This proposal was made obsolete by an "INFORM" message
        return;
    }

    const chain_node* next = new_config.next(m_us.token);

    if (!next || next->token != sender.token)
    {
        LOG(ERROR) << "dropping \"CONFIG_REJECT\" message that comes from the wrong place:"
                   << " " << sender << " instead of " << *next;
        return;
    }

    LOG(INFO) << "rejecting proposal " << proposal_id << ":" << proposal_time;
    m_fs.reject_configuration(proposal_id, proposal_time);
    m_config_manager.reject(proposal_id, proposal_time);
    const chain_node* prev = new_config.prev(m_us.token);

    if (prev)
    {
        SEND_CONFIG_RESP(*prev, REJECT, proposal_id, proposal_time, m_us);
    }
}

void
daemon :: accept_proposal(const chain_node& dest,
                          uint64_t proposal_id,
                          uint64_t proposal_time)
{
    m_fs.accept_configuration(proposal_id, proposal_time);
    SEND_CONFIG_RESP(dest, ACCEPT, proposal_id, proposal_time, m_us);
}

void
daemon :: reject_proposal(const chain_node& dest,
                          uint64_t proposal_id,
                          uint64_t proposal_time)
{
    m_fs.reject_configuration(proposal_id, proposal_time);
    SEND_CONFIG_RESP(dest, REJECT, proposal_id, proposal_time, m_us);
}

std::auto_ptr<e::buffer>
daemon :: create_inform_message()
{
    const configuration& config(m_config_manager.stable());
    size_t sz = BUSYBEE_HEADER_SIZE
              + pack_size(REPLNET_INFORM)
              + pack_size(config);
    std::auto_ptr<e::buffer> msg(e::buffer::create(sz));
    msg->pack_at(BUSYBEE_HEADER_SIZE) << REPLNET_INFORM << config;
    return msg;
}

void
daemon :: propose_config(const configuration& config)
{
    assert(config.cluster() == m_config_manager.latest().cluster());
    assert(config.version() == m_config_manager.latest().version() + 1);
    assert(config.validate());
    assert(config.prev_token() == m_config_manager.latest().this_token());
    assert(m_config_manager.contains_quorum_of_all(config));
    uint64_t proposal_id = 0xdeadbeefcafebabeULL;
    uint64_t proposal_time = e::time();
    generate_token(&proposal_id);
    std::vector<configuration> config_chain;
    m_config_manager.get_config_chain(&config_chain);
    config_chain.push_back(config);

    configuration* configs = &config_chain.front();
    size_t configs_sz = config_chain.size();
    assert(configs_sz > 1);
    assert(configs[configs_sz - 1] == config);
    assert(configs[configs_sz - 1].head()->token == m_us.token);
    m_fs.propose_configuration(proposal_id, proposal_time, configs, configs_sz);
    m_config_manager.merge(proposal_id, proposal_time, configs, configs_sz);
    LOG(INFO) << "proposing " << proposal_id << ":" << proposal_time << " " << config;

    if (configs[configs_sz - 1].config_tail()->token == m_us.token)
    {
        m_fs.accept_configuration(proposal_id, proposal_time);
        m_config_manager.advance(config);
        post_reconfiguration_hooks();
    }
    else
    {
        size_t sz = BUSYBEE_HEADER_SIZE
                  + pack_size(REPLNET_CONFIG_PROPOSE)
                  + 2 * sizeof(uint64_t)
                  + pack_size(m_us)
                  + pack_size(config_chain);
        std::auto_ptr<e::buffer> msg(e::buffer::create(sz));
        msg->pack_at(BUSYBEE_HEADER_SIZE) << REPLNET_CONFIG_PROPOSE
                                          << proposal_id << proposal_time
                                          << m_us << config_chain;
        const chain_node* next = config_chain.back().next(m_us.token);
        assert(next);
        send(*next, msg);
    }
}

void
daemon :: post_reconfiguration_hooks()
{
    trip_periodic(0, &daemon::periodic_maintain_cluster);
    po6::threads::mutex::hold hold(&m_bootstrap_mtx);
    m_bootstrap_cond.signal();
    m_have_bootstrapped = true;

    const configuration& config(m_config_manager.stable());
    LOG(INFO) << "deploying configuration " << config;

    // Check that our chain node has the correct address
    const chain_node* us = config.node_from_token(m_us.token);

    if (us && us->address != m_us.address)
    {
        trip_periodic(0, &daemon::periodic_change_address);
        m_have_bootstrapped = false;
    }

    // Inform all clients
    std::vector<uint64_t> clients;
    m_client_manager.list_clients(&clients);

    for (size_t i = 0; i < clients.size(); ++i)
    {
        send_no_disruption(clients[i], create_inform_message());
    }

    // Inform all cluster members
    for (const chain_node* n = config.members_begin();
            n != config.members_end(); ++n)
    {
        send(*n, create_inform_message());
    }

    // Inform temporary IDs
    for (std::map<uint64_t, uint64_t>::iterator it = m_temporary_servers.begin();
            it != m_temporary_servers.end(); )
    {
        if (it->second <= m_config_manager.stable().version())
        {
            send_no_disruption(it->first, create_inform_message());
            m_temporary_servers.erase(it);
            it = m_temporary_servers.begin();
        }
        else
        {
            ++it;
        }
    }

    // Heal chain commands
    reset_healing();

    const chain_node* tail = config.command_tail();

    if (tail && tail->token == m_us.token)
    {
        while (m_fs.next_slot_to_ack() < m_fs.next_slot_to_issue())
        {
            acknowledge_command(m_fs.next_slot_to_ack());
        }
    }

    if (!config.in_command_chain(m_us.token))
    {
        m_fs.clear_unacked_slots();
    }

    // Update the failure manager with full cluster membership
    update_failure_detectors();
    m_client_manager.proof_of_life(monotonic_time());

    // Log to let people know
    LOG(INFO) << "the latest stable configuration is " << m_config_manager.stable();
    LOG(INFO) << "the latest proposed configuration is " << m_config_manager.latest();
    uint64_t f_d = m_s.FAULT_TOLERANCE;
    uint64_t f_c = m_config_manager.stable().fault_tolerance();

    if (f_c < f_d)
    {
        LOG(WARNING) << "the most recently deployed configuration can tolerate at most "
                     << f_c << " failures which is less than the " << f_d
                     << " failures the cluster is expected to tolerate; "
                     << "bring " << m_config_manager.stable().servers_needed_for(f_d)
                     << " more servers online to restore "
                     << f_d << "-fault tolerance";
    }
    else
    {
        LOG(INFO) << "the most recently deployed configuration can tolerate the expected " << f_d << " failures";
    }
}

void
daemon :: background_bootstrap()
{
    uint64_t maintain_count = 0;

    while (true)
    {
        bool should_continue = true;
        bool have_bootstrap = true;
        m_bootstrap_mtx.lock();

        while (s_continue &&
               (m_have_bootstrapped ||
                maintain_count == m_maintain_count))
        {
            m_bootstrap_cond.wait();
        }

        should_continue = s_continue;
        have_bootstrap = m_have_bootstrapped;
        maintain_count = m_maintain_count;
        m_bootstrap_mtx.unlock();

        if (!should_continue)
        {
            break;
        }

        if (have_bootstrap)
        {
            continue;
        }

        for (size_t i = 0; i < m_bootstrap.size(); ++i)
        {
            chain_node cn;
            bootstrap_returncode rc;

            if ((rc = bootstrap_identity(m_bootstrap[i], &cn)) != BOOTSTRAP_SUCCESS)
            {
                continue;
            }

            if (cn.token == m_us.token)
            {
                continue;
            }

            size_t sz = BUSYBEE_HEADER_SIZE
                      + pack_size(REPLNET_SERVER_IDENTIFY);
            std::auto_ptr<e::buffer> msg(e::buffer::create(sz));
            msg->pack_at(BUSYBEE_HEADER_SIZE) << REPLNET_SERVER_IDENTIFY;
            send(cn, msg);
        }
    }
}

void
daemon :: periodic_change_address(uint64_t now)
{
    const chain_node* us = m_config_manager.latest().node_from_token(m_us.token);

    if (!us || us->address == m_us.address)
    {
        return;
    }

    const chain_node* head = m_config_manager.latest().head();
    trip_periodic(now + m_s.CHANGE_ADDRESS_INTERVAL, &daemon::periodic_change_address);

    if (head->token == us->token)
    {
        LOG(INFO) << "address in latest configuration has this node listed as accessible at "
                  << us->address << " but it is bound to " << m_us.address
                  << "; proposing a new configuration with an up-to-date address";
        configuration new_config = m_config_manager.latest();
        new_config.change_address(m_us.token, m_us.address);
        new_config.bump_version();
        propose_config(new_config);
    }
    else
    {
        LOG(INFO) << "address in latest configuration has this node listed as accessible at "
                  << us->address << " but it is bound to " << m_us.address
                  << "; sending a request to " << *head << " to update this node's address";

        size_t sz = BUSYBEE_HEADER_SIZE
                  + pack_size(REPLNET_SERVER_CHANGE_ADDRESS)
                  + pack_size(us->address)
                  + pack_size(m_us.address);
        std::auto_ptr<e::buffer> msg(e::buffer::create(sz));
        msg->pack_at(BUSYBEE_HEADER_SIZE) << REPLNET_SERVER_CHANGE_ADDRESS << us->address << m_us.address;
        send(*head, msg);
    }
}

void
daemon :: periodic_maintain_cluster(uint64_t now)
{
    trip_periodic(next_interval(now, m_s.FAILURE_DETECT_INTERVAL) + m_s.FAILURE_DETECT_SUSPECT_OFFSET,
                  &daemon::periodic_maintain_cluster);
    m_bootstrap_mtx.lock();
    ++m_maintain_count;
    m_bootstrap_cond.signal();
    m_bootstrap_mtx.unlock();

    if (!m_config_manager.stable().in_command_chain(m_us.token))
    {
        return;
    }

    const chain_node* nodes = m_config_manager.stable().members_begin();
    const chain_node* end = m_config_manager.stable().members_end();
    std::vector<uint64_t> tokens;
    size_t cutoff = 0;

    for (ssize_t i = 0; i < end - nodes; ++i)
    {
        if (nodes[i].token == m_us.token)
        {
            continue;
        }

        tokens.push_back(nodes[i].token);
    }

    m_failure_manager.get_suspicions(now, &tokens, &cutoff);

    // separate the servers into sets of servers:
    // stable_live:  servers that are in every config and not suspected
    // stable_dead:  servers that are in every config and suspected
    // unstable_live:  servers that are not in every config and not suspected
    // unstable_dead:  servers that are not in every config and suspected
    // removed_live:  servers that are not in any config and not suspected
    // removed_dead:  servers that are not in any config and suspected
    std::vector<uint64_t> stable_live_tokens;
    std::vector<uint64_t> stable_dead_tokens;
    std::vector<uint64_t> unstable_live_tokens;
    std::vector<uint64_t> unstable_dead_tokens;
    std::vector<uint64_t> removed_live_tokens;
    std::vector<uint64_t> removed_dead_tokens;
    stable_live_tokens.push_back(m_us.token);

    for (size_t i = 0; i < cutoff; ++i)
    {
        if (m_config_manager.all(&configuration::in_config_chain, tokens[i]))
        {
            stable_live_tokens.push_back(tokens[i]);
        }
        else if (m_config_manager.any(&configuration::in_config_chain, tokens[i]))
        {
            unstable_live_tokens.push_back(tokens[i]);
        }
        else
        {
            removed_live_tokens.push_back(tokens[i]);
        }
    }

    for (size_t i = cutoff; i < tokens.size(); ++i)
    {
        if (m_config_manager.all(&configuration::in_config_chain, tokens[i]))
        {
            stable_dead_tokens.push_back(tokens[i]);
        }
        else if (m_config_manager.any(&configuration::in_config_chain, tokens[i]))
        {
            unstable_dead_tokens.push_back(tokens[i]);
        }
        else
        {
            removed_dead_tokens.push_back(tokens[i]);
        }
    }

    assert(stable_live_tokens.size() + stable_dead_tokens.size() +
           unstable_live_tokens.size() + unstable_dead_tokens.size() +
           removed_live_tokens.size() + removed_dead_tokens.size()
           == tokens.size() + 1);

    // Here we have a decision to make that balances safety from future failures
    // with liveness now.  Dead nodes must be removed so that the system can
    // make progress.  Every time a node is removed, the resulting configuration
    // can tolerate fewer failures.  Our solution is to set a threshold that
    // limits the number of nodes that may be removed.  If we cannot provide
    // create a live chain within that threshold, we do nothing.
    //
    // Note that this calculation could be made faster by grouping or not
    // computing some of the groups above.  rescrv explicitly chose to not do so
    // for sake of clarity.  It's a small price to pay for comprehending this
    // code.

    if (stable_live_tokens.size() * 2 <= m_config_manager.smallest_config_chain())
    {
        LOG_EVERY_N(INFO, static_cast<int64_t>(SECONDS / m_s.FAILURE_DETECT_INTERVAL))
            << "could not propose new configuration because only "
            << stable_live_tokens.size() << " nodes are stable, which is not a quorum of "
            << m_config_manager.smallest_config_chain();
        return;
    }

    configuration new_config(m_config_manager.latest());

    // remove the stable dead nodes
    for (size_t i = 0; i < stable_dead_tokens.size(); ++i)
    {
        new_config.remove_from_chain(stable_dead_tokens[i]);
    }

    // remove the unstable dead nodes
    for (size_t i = 0; i < unstable_dead_tokens.size(); ++i)
    {
        if (new_config.in_config_chain(unstable_dead_tokens[i]))
        {
            new_config.remove_from_chain(unstable_dead_tokens[i]);
        }
    }

    // add nodes from the unstable/removed live nodes, preferring unstable,
    // sorted by suspicion.  Note that because suspsicions was sorted by
    // suspicion, and *_live_tokens were derived from it, the sort order is
    // implicitly captured by a forward iteration.
    uint64_t desired_size = 2 * m_s.FAULT_TOLERANCE + 1;

    for (size_t i = 0; i < unstable_live_tokens.size(); ++i)
    {
        if (new_config.config_size() < desired_size &&
            !new_config.in_config_chain(unstable_live_tokens[i]))
        {
            new_config.add_to_chain(unstable_live_tokens[i]);
        }
    }

    for (size_t i = 0; i < removed_live_tokens.size(); ++i)
    {
        if (new_config.config_size() < desired_size &&
            !new_config.in_config_chain(removed_live_tokens[i]))
        {
            new_config.add_to_chain(removed_live_tokens[i]);
        }
    }

    if (new_config.head()->token != m_us.token)
    {
        return;
    }

    if (new_config == m_config_manager.latest())
    {
        // promote people once the config chain stabilizes
        if (m_config_manager.stable().version() == m_config_manager.latest().version() &&
            m_config_manager.stable().version() == m_stable_version &&
            // checked above ^ *m_config_manager.latest().head() == m_us &&
            m_config_manager.latest().command_size() < m_config_manager.latest().config_size())
        {
            configuration grow_config(m_config_manager.latest());
            LOG(INFO) << "growing command chain to include more of the config chain by promoting "
                      << *m_config_manager.latest().next(m_config_manager.latest().command_tail()->token);
            grow_config.bump_version();
            grow_config.grow_command_chain();
            propose_config(grow_config);
        }

        return;
    }

    new_config.bump_version();

    if (!new_config.validate())
    {
        LOG_EVERY_N(INFO, static_cast<int64_t>(SECONDS / m_s.FAILURE_DETECT_INTERVAL))
            << "cannot propose " << new_config << " because it is invalid";
        return;
    }

    const configuration* no_quorum = NULL;

    if (!m_config_manager.contains_quorum_of_all(new_config, &no_quorum))
    {
        LOG_EVERY_N(INFO, static_cast<int64_t>(SECONDS / m_s.FAILURE_DETECT_INTERVAL))
            << "cannot propose " << new_config << " because it violates quorum invariants with "
            << *no_quorum;
        return;
    }

    if (m_config_manager.stable().config_tail()->token != m_us.token ||
        m_config_manager.stable().version() == m_stable_version)
    {
        LOG(INFO) << "proposing new configuration " << new_config;
        propose_config(new_config);
    }
}

void
daemon :: process_condition_wait(const replicant::connection& conn,
                                 std::auto_ptr<e::buffer>,
                                 e::unpacker up)
{
    uint64_t nonce;
    uint64_t object;
    uint64_t cond;
    uint64_t state;
    up = up >> nonce >> object >> cond >> state;
    CHECK_UNPACK(CONDITION_WAIT, up);

    if (!conn.is_client)
    {
        LOG(WARNING) << "rejecting \"CONDITION_WAIT\" that did not come from a client";
        size_t sz = BUSYBEE_HEADER_SIZE + pack_size(REPLNET_CLIENT_UNKNOWN);
        std::auto_ptr<e::buffer> msg(e::buffer::create(sz));
        msg->pack_at(BUSYBEE_HEADER_SIZE) << REPLNET_CLIENT_UNKNOWN;
        send_no_disruption(conn.token, msg);
        return;
    }
    else if (!conn.is_live_client)
    {
        LOG(WARNING) << "rejecting \"CONDITION_WAIT\" that came from dead client " << conn.token;
        size_t sz = BUSYBEE_HEADER_SIZE + pack_size(REPLNET_CLIENT_DECEASED);
        std::auto_ptr<e::buffer> msg(e::buffer::create(sz));
        msg->pack_at(BUSYBEE_HEADER_SIZE) << REPLNET_CLIENT_DECEASED;
        send_no_disruption(conn.token, msg);
        return;
    }

    m_object_manager.wait(object, conn.token, nonce, cond, state);
}

void
daemon :: process_client_register(const replicant::connection& conn,
                                  std::auto_ptr<e::buffer>,
                                  e::unpacker up)
{
    uint64_t client;
    up = up >> client;
    CHECK_UNPACK(CLIENT_REGISTER, up);
    bool success = true;

    if (conn.is_cluster_member)
    {
        LOG(WARNING) << "rejecting registration for client that comes from a cluster member";
        success = false;
    }

    if (conn.is_client)
    {
        LOG(WARNING) << "rejecting registration for client that comes from an existing client";
        success = false;
    }

    if (conn.token != client)
    {
        LOG(WARNING) << "rejecting registration for client (" << client
                     << ") that does not match its token (" << conn.token << ")";
        success = false;
    }

    const chain_node* head = m_config_manager.stable().head();

    if (!head || head->token != m_us.token)
    {
        LOG(WARNING) << "rejecting registration for client because we are not the head";
        success = false;
    }

    if (!success)
    {
        replicant::response_returncode rc = replicant::RESPONSE_REGISTRATION_FAIL;
        size_t sz = BUSYBEE_HEADER_SIZE
                  + pack_size(REPLNET_COMMAND_RESPONSE)
                  + sizeof(uint64_t)
                  + pack_size(rc);
        std::auto_ptr<e::buffer> msg(e::buffer::create(sz));
        msg->pack_at(BUSYBEE_HEADER_SIZE) << REPLNET_COMMAND_RESPONSE << uint64_t(0) << rc;
        send(conn, msg);
        return;
    }

    uint64_t slot = m_fs.next_slot_to_issue();
    issue_command(slot, OBJECT_CLI_REG, client, 0, e::slice("", 0));
}

void
daemon :: process_client_disconnect(const replicant::connection& conn,
                                    std::auto_ptr<e::buffer>,
                                    e::unpacker up)
{
    uint64_t nonce;
    up = up >> nonce;
    CHECK_UNPACK(CLIENT_DISCONNECT, up);

    if (!conn.is_client)
    {
        LOG(WARNING) << "rejecting \"CLIENT_DISCONNECT\" that doesn't come from a client";
        return;
    }

    const chain_node* head = m_config_manager.stable().head();

    if (!head || head->token != m_us.token)
    {
        LOG(WARNING) << "rejecting \"CLIENT_DISCONNECT\" because we are not the head";
        return;
    }

    uint64_t slot = m_fs.next_slot_to_issue();
    issue_command(slot, OBJECT_CLI_DIE, conn.token, nonce, e::slice("", 0));
}

void
daemon :: process_client_timeout(const replicant::connection& conn,
                                 std::auto_ptr<e::buffer>,
                                 e::unpacker up)
{
    uint64_t version;
    uint64_t client;
    up = up >> version >> client;
    CHECK_UNPACK(CLIENT_TIMEOUT, up);

    if (!conn.is_cluster_member)
    {
        LOG(WARNING) << "rejecting \"CLIENT_TIMEOUT\" from " << conn.token
                     << "/" << conn.addr << " which is not a cluster member";
        return;
    }

    const chain_node* head = m_config_manager.stable().head();

    if (!head || head->token != m_us.token ||
        version != m_config_manager.stable().version())
    {
        // silently drop because the sender is obligated to retry
        return;
    }

    uint64_t slot = m_fs.next_slot_to_issue();
    issue_command(slot, OBJECT_CLI_DIE, client, UINT64_MAX, e::slice("", 0));
}

void
daemon :: process_command_submit(const replicant::connection& conn,
                                 std::auto_ptr<e::buffer> msg,
                                 e::unpacker up)
{
    uint64_t object;
    uint64_t client;
    uint64_t nonce;
    up = up >> object >> client >> nonce;
    CHECK_UNPACK(COMMAND_SUBMIT, up);
    e::slice data = up.as_slice();

    // Check for special objects that a client tries to affect directly
    if (object != OBJECT_OBJ_NEW && object != OBJECT_OBJ_DEL &&
        object != OBJECT_OBJ_SNAPSHOT && object != OBJECT_OBJ_RESTORE &&
        IS_SPECIAL_OBJECT(object) && !conn.is_cluster_member)
    {
        LOG(INFO) << "rejecting \"COMMAND_SUBMIT\" for special object that "
                  << "was not sent by a cluster member";
        return;
    }

    if (!(conn.is_cluster_member || (conn.is_client && conn.is_live_client)))
    {
        if (conn.is_client && !conn.is_live_client)
        {
            LOG(INFO) << "rejecting \"COMMAND_SUBMIT\" from " << conn.token
                      << " because it is dead";
            size_t sz = BUSYBEE_HEADER_SIZE + pack_size(REPLNET_CLIENT_DECEASED);
            msg.reset(e::buffer::create(sz));
            msg->pack_at(BUSYBEE_HEADER_SIZE) << REPLNET_CLIENT_DECEASED;
            send_no_disruption(conn.token, msg);
            return;
        }
        else
        {
            LOG(INFO) << "rejecting \"COMMAND_SUBMIT\" from " << conn.token
                      << " because it is not a client or cluster member";
            size_t sz = BUSYBEE_HEADER_SIZE + pack_size(REPLNET_CLIENT_UNKNOWN);
            msg.reset(e::buffer::create(sz));
            msg->pack_at(BUSYBEE_HEADER_SIZE) << REPLNET_CLIENT_UNKNOWN;
            send_no_disruption(conn.token, msg);
            return;
        }
    }

    if (conn.is_client && conn.token != client)
    {
        LOG(INFO) << "rejecting \"COMMAND_SUBMIT\" from " << conn.token
                  << " because it uses the wrong token";
        size_t sz = BUSYBEE_HEADER_SIZE + pack_size(REPLNET_CLIENT_UNKNOWN);
        msg.reset(e::buffer::create(sz));
        msg->pack_at(BUSYBEE_HEADER_SIZE) << REPLNET_CLIENT_UNKNOWN;
        send_no_disruption(conn.token, msg);
        return;
    }

    uint64_t slot = 0;

    if (m_fs.get_slot(client, nonce, &slot))
    {
        assert(slot > 0);
        replicant::response_returncode rc;
        std::string backing;

        if (m_fs.get_exec(slot, &rc, &data, &backing))
        {
            size_t sz = BUSYBEE_HEADER_SIZE
                      + pack_size(REPLNET_COMMAND_RESPONSE)
                      + sizeof(uint64_t)
                      + pack_size(rc)
                      + data.size();
            msg.reset(e::buffer::create(sz));
            e::buffer::packer pa = msg->pack_at(BUSYBEE_HEADER_SIZE);
            pa = pa << REPLNET_COMMAND_RESPONSE << nonce << rc;
            pa = pa.copy(data);
            send_no_disruption(client, msg);
        }
        // else: drop it, it's proposed, but not executed

        return;
    }

    const chain_node* head = m_config_manager.stable().head();

    // If we are not the head
    if (!head || head->token != m_us.token)
    {
        // bounce the message
        send(*head, msg);
        return;
    }

    slot = m_fs.next_slot_to_issue();
    issue_command(slot, object, client, nonce, data);
}

void
daemon :: process_command_issue(const replicant::connection& conn,
                                std::auto_ptr<e::buffer>,
                                e::unpacker up)
{
    uint64_t slot = 0;
    uint64_t object = 0;
    uint64_t client = 0;
    uint64_t nonce = 0;
    up = up >> slot >> object >> client >> nonce;
    CHECK_UNPACK(COMMAND_ISSUE, up);
    e::slice data = up.as_slice();

    if (!conn.is_prev)
    {
        // just drop it, not from the right host
        return;
    }

    if (m_fs.is_acknowledged_slot(slot))
    {
        size_t sz = BUSYBEE_HEADER_SIZE
                  + pack_size(REPLNET_COMMAND_ACK)
                  + sizeof(uint64_t);
        std::auto_ptr<e::buffer> msg(e::buffer::create(sz));
        msg->pack_at(BUSYBEE_HEADER_SIZE) << REPLNET_COMMAND_ACK << slot;
        send(conn, msg);
        return;
    }

    const chain_node* tail = m_config_manager.stable().command_tail();

    if (m_fs.is_issued_slot(slot) && (!tail || tail->token != m_us.token))
    {
        // just drop it, we're waiting for an ACK ourselves
        return;
    }

    issue_command(slot, object, client, nonce, data);
}

void
daemon :: process_command_ack(const replicant::connection& conn,
                              std::auto_ptr<e::buffer>,
                              e::unpacker up)
{
    uint64_t slot = 0;
    up = up >> slot;
    CHECK_UNPACK(COMMAND_ACK, up);

    if (!conn.is_next)
    {
        // just drop it
        return;
    }

    if (!m_fs.is_issued_slot(slot))
    {
        LOG(WARNING) << "dropping \"COMMAND_ACK\" for slot that was not issued";
        return;
    }

    if (m_heal_next.state != heal_next::HEALTHY)
    {
        m_heal_next.acknowledged = slot + 1;
        transfer_more_state();
    }

    acknowledge_command(slot);
}

void
daemon :: issue_command(uint64_t slot,
                        uint64_t object,
                        uint64_t client,
                        uint64_t nonce,
                        const e::slice& data)
{
    if (slot != m_fs.next_slot_to_issue())
    {
        LOG(WARNING) << "dropping command issue that violates monotonicity "
                     << "slot=" << slot << " expected=" << m_fs.next_slot_to_issue();
        return;
    }

#ifdef REPL_LOG_COMMANDS
    LOG(INFO) << "ISSUE slot=" << slot
              << " object=" << object
              << " client=" << client
              << " nonce=" << nonce
              << " data=" << data.hex();
#endif

    m_fs.issue_slot(slot, object, client, nonce, data);
    m_client_manager.proof_of_life(client, monotonic_time());
    const chain_node* next = m_config_manager.stable().next(m_us.token);

    if (next)
    {
        if (m_heal_next.state >= heal_next::HEALTHY_SENT)
        {
            size_t sz = BUSYBEE_HEADER_SIZE
                      + pack_size(REPLNET_COMMAND_ISSUE)
                      + 4 * sizeof(uint64_t)
                      + data.size();
            std::auto_ptr<e::buffer> msg(e::buffer::create(sz));
            e::buffer::packer pa = msg->pack_at(BUSYBEE_HEADER_SIZE);
            pa = pa << REPLNET_COMMAND_ISSUE
                    << slot << object << client << nonce;
            pa.copy(data);
            send(*next, msg);
        }
    }

    const chain_node* tail = m_config_manager.stable().command_tail();

    if ((tail && tail->token == m_us.token) ||
        !m_config_manager.stable().in_command_chain(m_us.token))
    {
        acknowledge_command(slot);
    }
}

void
daemon :: defer_command(uint64_t object,
                        uint64_t client,
                        const e::slice& _data)
{
    e::compat::shared_ptr<e::buffer> data(e::buffer::create(_data.size()));
    data->resize(_data.size());
    memmove(data->data(), _data.data(), _data.size());

    {
        po6::threads::mutex::hold hold(&m_deferred_mtx);
        m_deferred->push(deferred_command(object, client, data));
    }

    trip_periodic(0, &daemon::periodic_execute_deferred);
}

void
daemon :: defer_command(uint64_t object,
                        uint64_t client, uint64_t nonce,
                        const e::slice& _data)
{
    e::compat::shared_ptr<e::buffer> data(e::buffer::create(_data.size()));
    data->resize(_data.size());
    memmove(data->data(), _data.data(), _data.size());

    {
        po6::threads::mutex::hold hold(&m_deferred_mtx);
        m_deferred->push(deferred_command(object, client, nonce, data));
    }

    trip_periodic(0, &daemon::periodic_execute_deferred);
}

void
daemon :: submit_command(uint64_t object,
                         uint64_t client, uint64_t nonce,
                         const e::slice& data)
{
    size_t sz = BUSYBEE_HEADER_SIZE
              + pack_size(REPLNET_COMMAND_SUBMIT)
              + 3 * sizeof(uint64_t)
              + data.size();
    std::auto_ptr<e::buffer> msg(e::buffer::create(sz));
    e::buffer::packer pa = msg->pack_at(BUSYBEE_HEADER_SIZE)
        << REPLNET_COMMAND_SUBMIT << object << client << nonce;
    pa.copy(data);

    const chain_node* head = m_config_manager.stable().head();

    if (head)
    {
        send(*head, msg);
    }
}

void
daemon :: acknowledge_command(uint64_t slot)
{
    if (m_fs.is_acknowledged_slot(slot))
    {
        // eliminate the dupe silently
        return;
    }

    if (slot != m_fs.next_slot_to_ack())
    {
        LOG(WARNING) << "dropping command ACK that violates monotonicity "
                     << "slot=" << slot << " expected=" << m_fs.next_slot_to_issue();
        return;
    }

    uint64_t object;
    uint64_t client;
    uint64_t nonce;
    e::slice data;
    std::string backing;

    if (!m_fs.get_slot(slot, &object, &client, &nonce, &data, &backing))
    {
        LOG(ERROR) << "cannot ack slot " << slot << " because there are gaps in our history (file a bug)";
        abort();
        return;
    }

#ifdef REPL_LOG_COMMANDS
    LOG(INFO) << "ACK slot=" << slot
              << " object=" << object
              << " client=" << client
              << " nonce=" << nonce
              << " data=" << data.hex();
#endif

    m_fs.ack_slot(slot);
    const chain_node* prev = m_config_manager.stable().prev(m_us.token);

    if (prev)
    {
        size_t sz = BUSYBEE_HEADER_SIZE
                  + pack_size(REPLNET_COMMAND_ACK)
                  + sizeof(uint64_t);
        std::auto_ptr<e::buffer> msg(e::buffer::create(sz));
        msg->pack_at(BUSYBEE_HEADER_SIZE) << REPLNET_COMMAND_ACK << slot;
        send(*prev, msg);
    }

    if (object == OBJECT_CLI_REG || object == OBJECT_CLI_DIE)
    {
        replicant::response_returncode rc = RESPONSE_SUCCESS;

        if (object == OBJECT_CLI_REG)
        {
            LOG(INFO) << "registering client " << client;
            m_fs.reg_client(client);
            m_client_manager.register_client(client);
            m_client_manager.proof_of_life(client, monotonic_time());
            update_failure_detectors();
        }
        else
        {
            LOG(INFO) << "disconnecting client " << client;
            m_fs.die_client(client);
            m_client_manager.deregister_client(client);
            update_failure_detectors();
        }

        size_t sz = BUSYBEE_HEADER_SIZE
                  + pack_size(REPLNET_COMMAND_RESPONSE)
                  + sizeof(uint64_t) + pack_size(replicant::RESPONSE_SUCCESS);
        std::auto_ptr<e::buffer> response(e::buffer::create(sz));
        response->pack_at(BUSYBEE_HEADER_SIZE) << REPLNET_COMMAND_RESPONSE << nonce << rc;
        send_no_disruption(client, response);
    }
    else
    {
        m_object_manager.enqueue(slot, object, client, nonce, data);
    }
}

void
daemon :: record_execution(uint64_t slot, uint64_t client, uint64_t nonce, replicant::response_returncode rc, const e::slice& data)
{
    size_t sz = BUSYBEE_HEADER_SIZE
              + pack_size(REPLNET_COMMAND_RESPONSE)
              + sizeof(uint64_t)
              + pack_size(rc)
              + data.size();
    std::auto_ptr<e::buffer> msg(e::buffer::create(sz));
    e::buffer::packer pa = msg->pack_at(BUSYBEE_HEADER_SIZE);
    pa = pa << REPLNET_COMMAND_RESPONSE << nonce << rc;
    pa = pa.copy(data);
    send_no_disruption(client, msg);
    m_fs.exec_slot(slot, rc, data);
}

void
daemon :: periodic_describe_slots(uint64_t now)
{
    trip_periodic(next_interval(now, m_s.REPORT_EVERY), &daemon::periodic_describe_slots);
    LOG(INFO) << "we are " << m_us << " and here's some info:"
              << " issued <=" << m_fs.next_slot_to_issue()
              << " | acked <=" << m_fs.next_slot_to_ack();
    LOG(INFO) << "our stable configuration is " << m_config_manager.stable();
    LOG(INFO) << "the suffix of the chain stabilized through " << m_stable_version;

    if (m_config_manager.stable().version() != m_config_manager.latest().version())
    {
        LOG(INFO) << "the latest outstanding configuration is " << m_config_manager.latest();
    }

    if (m_heal_next.state == heal_next::HEALING ||
        m_heal_next.state == heal_next::HEALTHY_SENT)
    {
        LOG(INFO) << "we've transfered through " << m_heal_next.acknowledged
                  << " and have begun transfer up to " << m_heal_next.proposed;
    }
}

void
daemon :: periodic_execute_deferred(uint64_t)
{
    while (!m_deferred->empty())
    {
        const deferred_command& dc(m_deferred->front());
        uint64_t slot = m_fs.next_slot_to_issue();

        if (dc.has_nonce)
        {
            issue_command(slot, dc.object, dc.client, dc.nonce, dc.data->as_slice());
        }
        else
        {
            issue_command(slot, dc.object, dc.client, slot, dc.data->as_slice());
        }

        m_deferred->pop();
    }
}

void
daemon :: process_heal_req(const replicant::connection& conn,
                           std::auto_ptr<e::buffer>,
                           e::unpacker up)
{
    uint64_t token;
    up = up >> token;
    CHECK_UNPACK(HEAL_REQ, up);

    const chain_node* prev = m_config_manager.stable().prev(m_us.token);

    if (!prev || prev->token != conn.token)
    {
        // just drop it
        return;
    }

    if (token <= m_heal_token)
    {
        size_t sz = BUSYBEE_HEADER_SIZE
                  + pack_size(REPLNET_HEAL_RETRY)
                  + sizeof(uint64_t);
        uint64_t new_token = m_heal_token + 1;
        std::auto_ptr<e::buffer> resp(e::buffer::create(sz));
        resp->pack_at(BUSYBEE_HEADER_SIZE) << REPLNET_HEAL_RETRY << new_token;
        send(conn, resp);
        LOG(INFO) << "received request for healing from our predecessor " << conn.token
                  << " with healing_id=" << token << ", but that token is too low;"
                  << " requesting a retry";
    }
    else
    {
        uint64_t to_ack = m_fs.next_slot_to_ack();
        size_t sz = BUSYBEE_HEADER_SIZE
                  + pack_size(REPLNET_HEAL_RESP)
                  + 2 * sizeof(uint64_t);
        std::auto_ptr<e::buffer> resp(e::buffer::create(sz));
        resp->pack_at(BUSYBEE_HEADER_SIZE) << REPLNET_HEAL_RESP << token << to_ack;
        send(conn, resp);
        LOG(INFO) << "resetting healing process with our predecessor " << conn.token
                  << ": healing_id=" << token << " to_ack=" << to_ack;
        maybe_send_stable();
    }
}

void
daemon :: process_heal_retry(const replicant::connection& conn,
                             std::auto_ptr<e::buffer>,
                             e::unpacker up)
{
    uint64_t token;
    up = up >> token;
    CHECK_UNPACK(HEAL_RETRY, up);
    LOG(INFO) << "received healing retry from successor " << conn.token
              << " with healing_id=" << token;
    m_heal_token = std::max(m_heal_token, token) + 1;
    reset_healing();
}

void
daemon :: process_heal_resp(const replicant::connection& conn,
                            std::auto_ptr<e::buffer>,
                            e::unpacker up)
{
    uint64_t token;
    uint64_t to_ack;
    up = up >> token >> to_ack;
    CHECK_UNPACK(HEAL_RESP, up);

    const chain_node* next = m_config_manager.stable().next(m_us.token);

    if (!next || next->token != conn.token ||
        token != m_heal_next.token)
    {
        // just drop it
        return;
    }

    // Process all acks up to, but not including, next to_ack
    while (m_fs.next_slot_to_ack() < m_fs.next_slot_to_issue() &&
           m_fs.next_slot_to_ack() < to_ack)
    {
        acknowledge_command(m_fs.next_slot_to_ack());
    }

    // take the min in case the next host is way ahead of us
    to_ack = std::min(to_ack, m_fs.next_slot_to_ack());
    m_heal_next.state = heal_next::HEALING;
    m_heal_next.acknowledged = to_ack;
    m_heal_next.proposed = to_ack;

    LOG(INFO) << "initiating state transfer to " << conn.token << " starting at slot " << to_ack;
    transfer_more_state();
}

void
daemon :: process_heal_done(const replicant::connection& conn,
                            std::auto_ptr<e::buffer> msg,
                            e::unpacker up)
{
    uint64_t token;
    up = up >> token;
    CHECK_UNPACK(HEAL_DONE, up);

    if (conn.is_next &&
        m_heal_next.token == token &&
        m_heal_next.state == heal_next::HEALTHY_SENT)
    {
        // we can move m_heal_next from HEALTHY_SENT to HEALTHY
        m_heal_next.state = heal_next::HEALTHY;
        LOG(INFO) << "the connection with the next node is 100% healed";
        const chain_node* tail = m_config_manager.stable().command_tail();

        if (tail && tail->token == m_us.token &&
            m_stable_version < m_config_manager.stable().version())
        {
            m_stable_version = m_config_manager.stable().version();
            LOG(INFO) << "command tail stabilizes at configuration " << m_stable_version;
        }
    }
    else if (conn.is_prev)
    {
        msg->pack_at(BUSYBEE_HEADER_SIZE) << REPLNET_HEAL_DONE << token;
        send(conn, msg);
        LOG(INFO) << "the connection with the prev node is 100% healed";
    }

    maybe_send_stable();
}

void
daemon :: process_stable(const replicant::connection&,
                         std::auto_ptr<e::buffer>,
                         e::unpacker up)
{
    uint64_t stable;
    up = up >> stable;
    CHECK_UNPACK(HEAL_DONE, up);

    if (m_stable_version < stable)
    {
        LOG(INFO) << "suffix of the chain (all nodes after us) stabilizes at configuration " << stable;
    }

    m_stable_version = std::max(m_stable_version, stable);
    maybe_send_stable();
}

void
daemon :: transfer_more_state()
{
    m_heal_next.window = m_fs.next_slot_to_issue() - m_heal_next.acknowledged;
    m_heal_next.window = std::max(m_heal_next.window, m_s.TRANSFER_WINDOW_LOWER_BOUND);
    m_heal_next.window = std::min(m_heal_next.window, m_s.TRANSFER_WINDOW_UPPER_BOUND);

    while (m_heal_next.state < heal_next::HEALTHY_SENT &&
           m_heal_next.proposed < m_fs.next_slot_to_issue() &&
           m_heal_next.proposed - m_heal_next.acknowledged <= m_heal_next.window)
    {
        uint64_t slot = m_heal_next.proposed;
        uint64_t object;
        uint64_t client;
        uint64_t nonce;
        e::slice data;
        std::string backing;

        if (!m_fs.get_slot(slot, &object, &client, &nonce, &data, &backing))
        {
            LOG(ERROR) << "cannot transfer slot " << m_heal_next.proposed << " because there are gaps in our history (file a bug)";
            abort();
        }

        size_t sz = BUSYBEE_HEADER_SIZE
                  + pack_size(REPLNET_COMMAND_ISSUE)
                  + 4 * sizeof(uint64_t)
                  + data.size();
        std::auto_ptr<e::buffer> msg(e::buffer::create(sz));
        e::buffer::packer pa = msg->pack_at(BUSYBEE_HEADER_SIZE);
        pa = pa << REPLNET_COMMAND_ISSUE << slot << object << client << nonce;
        pa.copy(data);
        const chain_node* next = m_config_manager.stable().next(m_us.token);
        assert(next);

        if (slot % 10000 == 0)
        {
            LOG(INFO) << "transferred through slot " << slot;
        }

        if (send(*next, msg))
        {
            ++m_heal_next.proposed;
        }
        else
        {
            break;
        }
    }

    if (m_heal_next.state < heal_next::HEALTHY_SENT &&
        m_heal_next.proposed == m_fs.next_slot_to_issue())
    {
        size_t sz = BUSYBEE_HEADER_SIZE
                  + pack_size(REPLNET_HEAL_DONE)
                  + sizeof(uint64_t);
        std::auto_ptr<e::buffer> msg(e::buffer::create(sz));
        msg->pack_at(BUSYBEE_HEADER_SIZE) << REPLNET_HEAL_DONE << m_heal_next.token;
        const chain_node* next = m_config_manager.stable().next(m_us.token);
        assert(next);

        if (send(*next, msg))
        {
            LOG(INFO) << "state transfer healing_id=" << m_heal_next.token << " complete; falling back to normal chain operation";
            m_heal_next.state = heal_next::HEALTHY_SENT;
            maybe_send_stable();
        }
    }
}

void
daemon :: periodic_heal_next(uint64_t now)
{
    const chain_node* next = m_config_manager.stable().next(m_us.token);

    // if there is no next node we're automatically healthy
    if (!next)
    {
        m_heal_next.state = heal_next::HEALTHY;

        // if we're the end of the command chain, report stability
        if (m_config_manager.stable().in_command_chain(m_us.token) &&
            m_stable_version < m_config_manager.stable().version())
        {
            m_stable_version = m_config_manager.stable().version();
            LOG(INFO) << "command tail stabilizes at configuration " << m_stable_version;
        }

        maybe_send_stable();
    }

    // keep running this function until we are healed
    if (m_heal_next.state != heal_next::HEALTHY)
    {
        trip_periodic(now + m_s.HEAL_NEXT_INTERVAL, &daemon::periodic_heal_next);
    }

    size_t sz;
    std::auto_ptr<e::buffer> msg;

    switch (m_heal_next.state)
    {
        case heal_next::BROKEN:
            assert(next);
            sz = BUSYBEE_HEADER_SIZE
               + pack_size(REPLNET_HEAL_REQ)
               + sizeof(uint64_t);
            msg.reset(e::buffer::create(sz));
            msg->pack_at(BUSYBEE_HEADER_SIZE) << REPLNET_HEAL_REQ
                                              << m_heal_token;

            if (send(*next, msg))
            {
                m_heal_next.state = heal_next::REQUEST_SENT;
                m_heal_next.token = m_heal_token;
                LOG(INFO) << "initiating healing with successor " << next->token
                          << " with healing_id=" << m_heal_token;
            }

            ++m_heal_token;
            break;
        case heal_next::REQUEST_SENT:
            m_heal_next.state = heal_next::BROKEN;
            ++m_heal_token;
            break;
        case heal_next::HEALING:
        case heal_next::HEALTHY_SENT:
            // do nothing, wait for other side
            break;
        case heal_next::HEALTHY:
            // do nothing, we won't run this periodic func anymore
            break;
        default:
            abort();
    }
}

void
daemon :: reset_healing()
{
    m_heal_next = heal_next();
    trip_periodic(0, &daemon::periodic_heal_next);
}

void
daemon :: maybe_send_stable()
{
    if (m_heal_next.state < heal_next::HEALTHY_SENT)
    {
        return;
    }

    size_t sz = BUSYBEE_HEADER_SIZE
              + pack_size(REPLNET_STABLE)
              + sizeof(uint64_t);
    std::auto_ptr<e::buffer> msg(e::buffer::create(sz));
    msg->pack_at(BUSYBEE_HEADER_SIZE) << REPLNET_STABLE
                                      << m_stable_version;
    const chain_node* prev = m_config_manager.stable().prev(m_us.token);

    if (prev)
    {
        send(*prev, msg);
    }
}

void
daemon :: send_notify(uint64_t client, uint64_t nonce, replicant::response_returncode rc, const e::slice& data)
{
    size_t sz = BUSYBEE_HEADER_SIZE
              + pack_size(REPLNET_CONDITION_NOTIFY)
              + sizeof(uint64_t)
              + pack_size(rc)
              + data.size();
    std::auto_ptr<e::buffer> msg(e::buffer::create(sz));
    e::buffer::packer pa = msg->pack_at(BUSYBEE_HEADER_SIZE);
    pa = pa << REPLNET_CONDITION_NOTIFY << nonce << rc;
    pa = pa.copy(data);
    send_no_disruption(client, msg);
}

void
daemon :: handle_snapshot(std::auto_ptr<snapshot>)
{
}

void
daemon :: process_ping(const replicant::connection& conn,
                       std::auto_ptr<e::buffer> msg,
                       e::unpacker up)
{
    uint64_t version = 0;
    uint64_t seqno = 0;
    up = up >> version >> seqno;
    CHECK_UNPACK(PING, up);

    if (version < m_config_manager.stable().version())
    {
        size_t sz = BUSYBEE_HEADER_SIZE
                  + pack_size(REPLNET_INFORM)
                  + pack_size(m_config_manager.stable());
        std::auto_ptr<e::buffer> inf(e::buffer::create(sz));
        inf->pack_at(BUSYBEE_HEADER_SIZE) << REPLNET_INFORM << m_config_manager.stable();
        send(conn, inf);
    }

    msg->clear();
    msg->pack_at(BUSYBEE_HEADER_SIZE) << REPLNET_PONG << seqno;
    send(conn, msg);
}

void
daemon :: process_pong(const replicant::connection& conn,
                       std::auto_ptr<e::buffer>,
                       e::unpacker up)
{
    uint64_t seqno = 0;
    up = up >> seqno;
    CHECK_UNPACK(PONG, up);
    uint64_t now = monotonic_time();
    m_failure_manager.pong(conn.token, seqno, now);

    if (conn.is_client)
    {
        m_client_manager.proof_of_life(conn.token, now);
    }
}

void
daemon :: periodic_exchange(uint64_t now)
{
    trip_periodic(next_interval(now, m_s.FAILURE_DETECT_INTERVAL) + m_s.FAILURE_DETECT_PING_OFFSET,
                  &daemon::periodic_exchange);
    uint64_t version = m_config_manager.stable().version();
    m_failure_manager.ping(this, &daemon::send_no_disruption, version);
}

void
daemon :: periodic_suspect_clients(uint64_t now)
{
    trip_periodic(next_interval(now, m_s.FAILURE_DETECT_INTERVAL) + m_s.FAILURE_DETECT_SUSPECT_OFFSET,
                  &daemon::periodic_suspect_clients);
    const configuration& config(m_config_manager.stable());
    std::vector<uint64_t> clients;
    m_client_manager.owned_clients(config.index(m_us.token),
                                   config.config_size(),
                                   &clients);
    size_t cutoff = 0;
    m_failure_manager.get_suspicions(now, &clients, &cutoff);

    for (size_t i = cutoff; i < clients.size(); ++i)
    {
        m_object_manager.suspect(clients[i]);
    }
}

void
daemon :: periodic_disconnect_clients(uint64_t now)
{
    trip_periodic(next_interval(now, m_s.CLIENT_DISCONNECT_EVERY),
                  &daemon::periodic_disconnect_clients);
    const configuration& config(m_config_manager.stable());
    std::vector<uint64_t> our_clients;
    m_client_manager.owned_clients(config.index(m_us.token),
                                   config.config_size(),
                                   &our_clients);
    std::vector<uint64_t> dead_clients;
    m_client_manager.last_seen_before(now - m_s.CLIENT_DISCONNECT_TIMEOUT,
                                      &dead_clients);
    uint64_t version = m_config_manager.stable().version();
    const chain_node* head = m_config_manager.stable().head();

    if (!head)
    {
        return;
    }

    for (size_t i = 0; i < our_clients.size(); ++i)
    {
        if (!std::binary_search(dead_clients.begin(),
                                dead_clients.end(),
                                our_clients[i]))
        {
            continue;
        }

        po6::net::location addr;

        if (m_busybee->get_addr(our_clients[i], &addr) == BUSYBEE_SUCCESS)
        {
            m_client_manager.proof_of_life(our_clients[i], now);
            continue;
        }

        size_t sz = BUSYBEE_HEADER_SIZE
                  + pack_size(REPLNET_CLIENT_TIMEOUT)
                  + sizeof(uint64_t)
                  + sizeof(uint64_t);
        std::auto_ptr<e::buffer> msg(e::buffer::create(sz));
        msg->pack_at(BUSYBEE_HEADER_SIZE)
            << REPLNET_CLIENT_TIMEOUT << version << our_clients[i];
        send_no_disruption(head->token, msg);
        LOG(INFO) << "session for " << our_clients[i] << " timed out";
    }

    std::vector<uint64_t> all_clients;
    m_client_manager.list_clients(&all_clients);
    m_object_manager.suspect_if_not_listed(all_clients);
}

void
daemon :: update_failure_detectors()
{
    std::vector<uint64_t> nodes;
    m_config_manager.get_all_nodes(&nodes);
    std::vector<uint64_t> clients;
    m_client_manager.list_clients(&clients);
    std::vector<uint64_t> ids(nodes.size() + clients.size());
    std::merge(nodes.begin(), nodes.end(),
               clients.begin(), clients.end(),
               ids.begin());

    for (size_t i = 0; i < ids.size(); ++i)
    {
        if (ids[i] != m_us.token)
        {
            continue;
        }

        for (size_t j = i + 1; j < ids.size(); ++j)
        {
            ids[j - 1] = ids[j];
        }

        ids.pop_back();
        break;
    }

    uint64_t now = monotonic_time();
    m_failure_manager.track(now, ids,
                            m_s.FAILURE_DETECT_INTERVAL,
                            m_s.FAILURE_DETECT_WINDOW_SIZE);
}

void
daemon :: issue_suspect_callback(uint64_t obj_id,
                                 uint64_t cb_id,
                                 const e::slice& data)
{
    submit_command(obj_id, CLIENT_SUSPECT, cb_id, data);
}

void
daemon :: periodic_alarm(uint64_t now)
{
    uint64_t ms250 = 250ULL * 1000ULL * 1000ULL;
    uint64_t when = now + ms250 - (now % ms250);
    trip_periodic(when, &daemon::periodic_alarm);

    if (m_config_manager.stable().head()->token != m_us.token)
    {
        return;
    }

    m_object_manager.periodic(now);
}

void
daemon :: issue_alarm(uint64_t obj_id, const char* func)
{
    if (m_config_manager.stable().head()->token != m_us.token)
    {
        LOG(ERROR) << "alarm lost (should not happen)";
        return;
    }

    e::slice data(func, strlen(func) + 1);
    defer_command(obj_id, CLIENT_ALARM, data);
}

bool
daemon :: recv(replicant::connection* conn, std::auto_ptr<e::buffer>* msg)
{
    while (s_continue)
    {
        run_periodic();
        busybee_returncode rc = m_busybee->recv(&conn->token, msg);

        switch (rc)
        {
            case BUSYBEE_SUCCESS:
                break;
            case BUSYBEE_TIMEOUT:
            case BUSYBEE_INTERRUPTED:
                continue;
            case BUSYBEE_DISRUPTED:
                handle_disruption(conn->token);
                continue;
            case BUSYBEE_SHUTDOWN:
            case BUSYBEE_POLLFAILED:
            case BUSYBEE_ADDFDFAIL:
            case BUSYBEE_EXTERNAL:
            default:
                LOG(ERROR) << "BusyBee returned " << rc << " during a \"recv\" call";
                return false;
        }

        if (m_busybee->get_addr(conn->token, &conn->addr) != BUSYBEE_SUCCESS)
        {
            conn->addr = po6::net::location();
        }

        const configuration& config(m_config_manager.stable());

        for (const chain_node* n = config.members_begin();
                n != config.members_end(); ++n)
        {
            if (n->token == conn->token)
            {
                const chain_node* prev = config.prev(m_us.token);
                const chain_node* next = config.next(m_us.token);
                conn->is_cluster_member = true;
                conn->is_client = false;
                conn->is_prev = prev && n->token == prev->token;
                conn->is_next = next && n->token == next->token;
                return true;
            }
        }

        conn->is_cluster_member = false;
        conn->is_client = m_fs.lookup_client(conn->token, &conn->is_live_client);
        conn->is_prev = false;
        conn->is_next = false;
        return true;
    }

    return false;
}

bool
daemon :: send(const replicant::connection& conn, std::auto_ptr<e::buffer> msg)
{
    po6::threads::mutex::hold hold(&m_send_mtx);

    if (m_disrupted_backoff.find(conn.token) != m_disrupted_backoff.end())
    {
        return false;
    }

    switch (m_busybee->send(conn.token, msg))
    {
        case BUSYBEE_SUCCESS:
            return true;
        case BUSYBEE_DISRUPTED:
            handle_disruption(conn.token);
            return false;
        case BUSYBEE_SHUTDOWN:
        case BUSYBEE_POLLFAILED:
        case BUSYBEE_ADDFDFAIL:
        case BUSYBEE_TIMEOUT:
        case BUSYBEE_EXTERNAL:
        case BUSYBEE_INTERRUPTED:
        default:
            return false;
    }
}

bool
daemon :: send(const chain_node& node, std::auto_ptr<e::buffer> msg)
{
    po6::threads::mutex::hold hold(&m_send_mtx);

    if (m_disrupted_backoff.find(node.token) != m_disrupted_backoff.end())
    {
        return false;
    }

    m_busybee_mapper.set(node);

    switch (m_busybee->send(node.token, msg))
    {
        case BUSYBEE_SUCCESS:
            return true;
        case BUSYBEE_DISRUPTED:
            handle_disruption(node.token);
            return false;
        case BUSYBEE_SHUTDOWN:
        case BUSYBEE_POLLFAILED:
        case BUSYBEE_ADDFDFAIL:
        case BUSYBEE_TIMEOUT:
        case BUSYBEE_EXTERNAL:
        case BUSYBEE_INTERRUPTED:
        default:
            return false;
    }
}

bool
daemon :: send_no_disruption(uint64_t token, std::auto_ptr<e::buffer> msg)
{
    const chain_node* node = m_config_manager.latest().node_from_token(token);

    if (node)
    {
        m_busybee_mapper.set(*node);
    }

    switch (m_busybee->send(token, msg))
    {
        case BUSYBEE_SUCCESS:
            return true;
        case BUSYBEE_DISRUPTED:
            return false;
        case BUSYBEE_SHUTDOWN:
        case BUSYBEE_POLLFAILED:
        case BUSYBEE_ADDFDFAIL:
        case BUSYBEE_TIMEOUT:
        case BUSYBEE_EXTERNAL:
        case BUSYBEE_INTERRUPTED:
        default:
            return false;
    }
}

void
daemon :: handle_disruption(uint64_t token)
{
    m_disrupted_backoff.insert(token);

    if (!m_disrupted_retry_scheduled)
    {
        trip_periodic(e::time() + m_s.CONNECTION_RETRY, &daemon::periodic_handle_disruption);
        m_disrupted_retry_scheduled = true;
    }
}

void
daemon :: periodic_handle_disruption(uint64_t now)
{
    m_disrupted_retry_scheduled = false;
    std::set<uint64_t> disrupted_backoff = m_disrupted_backoff;
    m_disrupted_backoff.clear();

    for (std::set<uint64_t>::iterator it = disrupted_backoff.begin();
            it != disrupted_backoff.end(); ++it)
    {
        uint64_t token = *it;
        handle_disruption_reset_reconfiguration(token);
        handle_disruption_reset_healing(token);
    }

    if (!m_disrupted_backoff.empty())
    {
        trip_periodic(now + m_s.CONNECTION_RETRY, &daemon::periodic_handle_disruption);
        m_disrupted_retry_scheduled = true;
    }
}

void
daemon :: handle_disruption_reset_healing(uint64_t token)
{
    const chain_node* next = m_config_manager.stable().next(m_us.token);

    if (next)
    {
        if (token == next->token)
        {
            reset_healing();
        }
    }

    maybe_send_stable();
}

void
daemon :: handle_disruption_reset_reconfiguration(uint64_t token)
{
    std::vector<configuration> config_chain;
    std::vector<configuration_manager::proposal> proposals;
    m_config_manager.get_config_chain(&config_chain);
    m_config_manager.get_proposals(&proposals);

    for (size_t i = 0; i < config_chain.size(); ++ i)
    {
        const chain_node* next = config_chain[i].next(m_us.token);

        if (!next || next->token != token)
        {
            continue;
        }

        configuration_manager::proposal* prop = NULL;

        for (size_t j = 0; j < proposals.size(); ++j)
        {
            if (proposals[j].version == config_chain[i].version())
            {
                prop = &proposals[j];
                break;
            }
        }

        if (!prop)
        {
            continue;
        }

        std::vector<configuration> cc(config_chain.begin(), config_chain.begin() + i + 1);
        size_t sz = BUSYBEE_HEADER_SIZE
                  + pack_size(REPLNET_CONFIG_PROPOSE)
                  + 2 * sizeof(uint64_t)
                  + pack_size(m_us)
                  + pack_size(cc);
        std::auto_ptr<e::buffer> msg(e::buffer::create(sz));
        msg->pack_at(BUSYBEE_HEADER_SIZE) << REPLNET_CONFIG_PROPOSE
                                          << prop->id << prop->time
                                          << m_us << cc;
        send(*next, msg);
    }
}

typedef void (daemon::*_periodic_fptr)(uint64_t now);
typedef std::pair<uint64_t, _periodic_fptr> _periodic;

static bool
compare_periodic(const _periodic& lhs, const _periodic& rhs)
{
    return lhs.first > rhs.first;
}

void
daemon :: trip_periodic(uint64_t when, periodic_fptr fp)
{
    po6::threads::mutex::hold hold(&m_periodic_mtx);

    for (size_t i = 0; i < m_periodic.size(); ++i)
    {
        if (m_periodic[i].second == fp)
        {
            m_periodic[i].second = &daemon::periodic_nop;
        }
    }

    // Clean up dead functions from the front
    while (!m_periodic.empty() && m_periodic[0].second == &daemon::periodic_nop)
    {
        std::pop_heap(m_periodic.begin(), m_periodic.end(), compare_periodic);
        m_periodic.pop_back();
    }

    // And from the back
    while (!m_periodic.empty() && m_periodic.back().second == &daemon::periodic_nop)
    {
        m_periodic.pop_back();
    }

    m_periodic.push_back(std::make_pair(when, fp));
    std::push_heap(m_periodic.begin(), m_periodic.end(), compare_periodic);
}

void
daemon :: run_periodic()
{
    po6::threads::mutex::hold hold(&m_periodic_mtx);
    uint64_t now = monotonic_time();

    while (!m_periodic.empty() && m_periodic[0].first <= now)
    {
        if (m_periodic.size() > m_s.PERIODIC_SIZE_WARNING)
        {
            LOG(WARNING) << "there are " << m_periodic.size()
                         << " functions scheduled which exceeds the threshold of "
                         << m_s.PERIODIC_SIZE_WARNING << " functions";
        }

        periodic_fptr fp;
        std::pop_heap(m_periodic.begin(), m_periodic.end(), compare_periodic);
        fp = m_periodic.back().second;
        m_periodic.pop_back();
        m_periodic_mtx.unlock();
        (this->*fp)(now);
        m_periodic_mtx.lock();
    }
}

void
daemon :: periodic_nop(uint64_t)
{
}

bool
daemon :: generate_token(uint64_t* token)
{
    po6::io::fd sysrand(open("/dev/urandom", O_RDONLY));

    if (sysrand.get() < 0)
    {
        return false;
    }

    *token = 0;

    while (*token < (1ULL << 32))
    {
        if (sysrand.read(token, sizeof(*token)) != sizeof(*token))
        {
            return false;
        }
    }

    return true;
}
