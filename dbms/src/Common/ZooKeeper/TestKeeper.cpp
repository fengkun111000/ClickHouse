#include <Common/ZooKeeper/TestKeeper.h>
#include <Common/setThreadName.h>
#include <Common/StringUtils/StringUtils.h>
#include <Core/Types.h>

#include <sstream>
#include <iomanip>


namespace Coordination
{

static String parentPath(const String & path)
{
    auto rslash_pos = path.rfind('/');
    if (rslash_pos > 0)
        return path.substr(0, rslash_pos);
    return "/";
}

static String baseName(const String & path)
{
    auto rslash_pos = path.rfind('/');
    return path.substr(rslash_pos + 1);
}


struct TestKeeperRequest : virtual Request
{
    virtual bool isMutable() const { return false; }
    virtual ResponsePtr createResponse() const = 0;
    virtual ResponsePtr process(TestKeeper::Container & container, int64_t zxid) const = 0;
    virtual void processWatches(TestKeeper::Watches & /*watches*/, TestKeeper::Watches & /*list_watches*/) const {}
};


static void processWatchesImpl(const String & path, TestKeeper::Watches & watches, TestKeeper::Watches & list_watches)
{
    WatchResponse watch_response;
    watch_response.path = path;

    auto it = watches.find(watch_response.path);
    if (it != watches.end())
    {
        for (auto & callback : it->second)
            if (callback)
                callback(watch_response);

        watches.erase(it);
    }

    WatchResponse watch_list_response;
    watch_list_response.path = parentPath(path);

    it = list_watches.find(watch_list_response.path);
    if (it != list_watches.end())
    {
        for (auto & callback : it->second)
            if (callback)
                callback(watch_list_response);

        list_watches.erase(it);
    }
}


struct TestKeeperCreateRequest final : CreateRequest, TestKeeperRequest
{
    TestKeeperCreateRequest() {}
    TestKeeperCreateRequest(const CreateRequest & base) : CreateRequest(base) {}
    ResponsePtr createResponse() const override;
    ResponsePtr process(TestKeeper::Container & container, int64_t zxid) const override;

    void processWatches(TestKeeper::Watches & node_watches, TestKeeper::Watches & list_watches) const override
    {
        processWatchesImpl(getPath(), node_watches, list_watches);
    }
};

struct TestKeeperRemoveRequest final : RemoveRequest, TestKeeperRequest
{
    TestKeeperRemoveRequest() {}
    TestKeeperRemoveRequest(const RemoveRequest & base) : RemoveRequest(base) {}
    bool isMutable() const override { return true; }
    ResponsePtr createResponse() const override;
    ResponsePtr process(TestKeeper::Container & container, int64_t zxid) const override;

    void processWatches(TestKeeper::Watches & node_watches, TestKeeper::Watches & list_watches) const override
    {
        processWatchesImpl(getPath(), node_watches, list_watches);
    }
};

struct TestKeeperExistsRequest final : ExistsRequest, TestKeeperRequest
{
    ResponsePtr createResponse() const override;
    ResponsePtr process(TestKeeper::Container & container, int64_t zxid) const override;
};

struct TestKeeperGetRequest final : GetRequest, TestKeeperRequest
{
    TestKeeperGetRequest() {}
    ResponsePtr createResponse() const override;
    ResponsePtr process(TestKeeper::Container & container, int64_t zxid) const override;
};

struct TestKeeperSetRequest final : SetRequest, TestKeeperRequest
{
    TestKeeperSetRequest() {}
    TestKeeperSetRequest(const SetRequest & base) : SetRequest(base) {}
    bool isMutable() const override { return true; }
    ResponsePtr createResponse() const override;
    ResponsePtr process(TestKeeper::Container & container, int64_t zxid) const override;

    void processWatches(TestKeeper::Watches & node_watches, TestKeeper::Watches & list_watches) const override
    {
        processWatchesImpl(getPath(), node_watches, list_watches);
    }
};

struct TestKeeperListRequest final : ListRequest, TestKeeperRequest
{
    ResponsePtr createResponse() const override;
    ResponsePtr process(TestKeeper::Container & container, int64_t zxid) const override;
};

struct TestKeeperCheckRequest final : CheckRequest, TestKeeperRequest
{
    TestKeeperCheckRequest() {}
    TestKeeperCheckRequest(const CheckRequest & base) : CheckRequest(base) {}
    ResponsePtr createResponse() const override;
    ResponsePtr process(TestKeeper::Container & container, int64_t zxid) const override;
};

struct TestKeeperMultiRequest final : MultiRequest, TestKeeperRequest
{
    TestKeeperMultiRequest(const Requests & generic_requests)
    {
        requests.reserve(generic_requests.size());

        for (const auto & generic_request : generic_requests)
        {
            if (auto * concrete_request_create = dynamic_cast<const CreateRequest *>(generic_request.get()))
            {
                auto create = std::make_shared<TestKeeperCreateRequest>(*concrete_request_create);
                requests.push_back(create);
            }
            else if (auto * concrete_request_remove = dynamic_cast<const RemoveRequest *>(generic_request.get()))
            {
                requests.push_back(std::make_shared<TestKeeperRemoveRequest>(*concrete_request_remove));
            }
            else if (auto * concrete_request_set = dynamic_cast<const SetRequest *>(generic_request.get()))
            {
                requests.push_back(std::make_shared<TestKeeperSetRequest>(*concrete_request_set));
            }
            else if (auto * concrete_request_check = dynamic_cast<const CheckRequest *>(generic_request.get()))
            {
                requests.push_back(std::make_shared<TestKeeperCheckRequest>(*concrete_request_check));
            }
            else
                throw Exception("Illegal command as part of multi ZooKeeper request", ZBADARGUMENTS);
        }
    }

    void processWatches(TestKeeper::Watches & node_watches, TestKeeper::Watches & list_watches) const override
    {
        for (const auto & generic_request : requests)
            dynamic_cast<const TestKeeperRequest &>(*generic_request).processWatches(node_watches, list_watches);
    }

    ResponsePtr createResponse() const override;
    ResponsePtr process(TestKeeper::Container & container, int64_t zxid) const override;
};


ResponsePtr TestKeeperCreateRequest::process(TestKeeper::Container & container, int64_t zxid) const
{
    CreateResponse response;
    if (container.count(path))
    {
        response.error = Error::ZNODEEXISTS;
    }
    else
    {
        auto it = container.find(parentPath(path));

        if (it == container.end())
        {
            response.error = Error::ZNONODE;
        }
        else if (it->second.is_ephemeral)
        {
            response.error = Error::ZNOCHILDRENFOREPHEMERALS;
        }
        else
        {
            TestKeeper::Node created_node;
            created_node.seq_num = 0;
            created_node.stat.czxid = zxid;
            created_node.stat.mzxid = zxid;
            created_node.stat.ctime = std::chrono::system_clock::now().time_since_epoch() / std::chrono::milliseconds(1);
            created_node.stat.mtime = created_node.stat.ctime;
            created_node.stat.numChildren = 0;
            created_node.stat.dataLength = data.length();
            created_node.data = data;
            created_node.is_ephemeral = is_ephemeral;
            created_node.is_sequental = is_sequential;
            std::string path_created = path;

            if (is_sequential)
            {
                auto seq_num = it->second.seq_num;
                ++it->second.seq_num;

                std::stringstream seq_num_str;
                seq_num_str << std::setw(10) << std::setfill('0') << seq_num;

                path_created += seq_num_str.str();
            }

            response.path_created = path_created;
            container.emplace(std::move(path_created), std::move(created_node));

            ++it->second.stat.cversion;
            ++it->second.stat.numChildren;

            response.error = Error::ZOK;
        }
    }

    return std::make_shared<CreateResponse>(response);
}

ResponsePtr TestKeeperRemoveRequest::process(TestKeeper::Container & container, int64_t) const
{
    RemoveResponse response;

    auto it = container.find(path);
    if (it == container.end())
    {
        response.error = Error::ZNONODE;
    }
    else if (version != -1 && version != it->second.stat.version)
    {
        response.error = Error::ZBADVERSION;
    }
    else if (it->second.stat.numChildren)
    {
        response.error = Error::ZNOTEMPTY;
    }
    else
    {
        container.erase(it);
        auto & parent = container.at(parentPath(path));
        --parent.stat.numChildren;
        ++parent.stat.cversion;
        response.error = Error::ZOK;
    }

    return std::make_shared<RemoveResponse>(response);
}

ResponsePtr TestKeeperExistsRequest::process(TestKeeper::Container & container, int64_t) const
{
    ExistsResponse response;

    auto it = container.find(path);
    if (it != container.end())
    {
        response.stat = it->second.stat;
        response.error = Error::ZOK;
    }
    else
    {
        response.error = Error::ZNONODE;
    }

    return std::make_shared<ExistsResponse>(response);
}

ResponsePtr TestKeeperGetRequest::process(TestKeeper::Container & container, int64_t) const
{
    GetResponse response;

    auto it = container.find(path);
    if (it == container.end())
    {
        response.error = Error::ZNONODE;
    }
    else
    {
        response.stat = it->second.stat;
        response.data = it->second.data;
        response.error = Error::ZOK;
    }

    return std::make_shared<GetResponse>(response);
}

ResponsePtr TestKeeperSetRequest::process(TestKeeper::Container & container, int64_t zxid) const
{
    SetResponse response;

    auto it = container.find(path);
    if (it == container.end())
    {
        response.error = Error::ZNONODE;
    }
    else if (version == -1 || version == it->second.stat.version)
    {
        it->second.data = data;
        ++it->second.stat.version;
        it->second.stat.mzxid = zxid;
        it->second.stat.mtime = std::chrono::system_clock::now().time_since_epoch() / std::chrono::milliseconds(1);
        it->second.data = data;
        ++container.at(parentPath(path)).stat.cversion;
        response.stat = it->second.stat;
        response.error = Error::ZOK;
    }
    else
    {
        response.error = Error::ZBADVERSION;
    }

    return std::make_shared<SetResponse>(response);
}

ResponsePtr TestKeeperListRequest::process(TestKeeper::Container & container, int64_t) const
{
    ListResponse response;

    auto it = container.find(path);
    if (it == container.end())
    {
        response.error = Error::ZNONODE;
    }
    else
    {
        auto path_prefix = path;
        if (path_prefix.empty())
            throw Exception("Logical error: path cannot be empty", ZSESSIONEXPIRED);

        if (path_prefix.back() != '/')
            path_prefix += '/';

        /// Fairly inefficient.
        for (auto child_it = container.upper_bound(path_prefix); child_it != container.end() && startsWith(child_it->first, path_prefix); ++child_it)
            if (parentPath(child_it->first) == path)
                response.names.emplace_back(baseName(child_it->first));

        response.stat = it->second.stat;
        response.error = Error::ZOK;
    }

    return std::make_shared<ListResponse>(response);
}

ResponsePtr TestKeeperCheckRequest::process(TestKeeper::Container & container, int64_t) const
{
    CheckResponse response;
    auto it = container.find(path);
    if (it == container.end())
    {
        response.error = Error::ZNONODE;
    }
    else if (version != -1 && version != it->second.stat.version)
    {
        response.error = Error::ZBADVERSION;
    }
    else
    {
        response.error = Error::ZOK;
    }

    return std::make_shared<CheckResponse>(response);
}

ResponsePtr TestKeeperMultiRequest::process(TestKeeper::Container & container, int64_t zxid) const
{
    MultiResponse response;
    response.responses.reserve(requests.size());

    /// Fairly inefficient.
    auto container_copy = container;

    try
    {
        for (const auto & request : requests)
        {
            const TestKeeperRequest & concrete_request = dynamic_cast<const TestKeeperRequest &>(*request);
            auto cur_response = concrete_request.process(container, zxid);
            response.responses.emplace_back(cur_response);
            if (cur_response->error != Error::ZOK)
            {
                response.error = cur_response->error;
                container = container_copy;
                return std::make_shared<MultiResponse>(response);
            }
        }

        response.error = Error::ZOK;
        return std::make_shared<MultiResponse>(response);
    }
    catch (...)
    {
        container = container_copy;
        throw;
    }
}

ResponsePtr TestKeeperCreateRequest::createResponse() const { return std::make_shared<CreateResponse>(); }
ResponsePtr TestKeeperRemoveRequest::createResponse() const { return std::make_shared<RemoveResponse>(); }
ResponsePtr TestKeeperExistsRequest::createResponse() const { return std::make_shared<ExistsResponse>(); }
ResponsePtr TestKeeperGetRequest::createResponse() const { return std::make_shared<GetResponse>(); }
ResponsePtr TestKeeperSetRequest::createResponse() const { return std::make_shared<SetResponse>(); }
ResponsePtr TestKeeperListRequest::createResponse() const { return std::make_shared<ListResponse>(); }
ResponsePtr TestKeeperCheckRequest::createResponse() const { return std::make_shared<CheckResponse>(); }
ResponsePtr TestKeeperMultiRequest::createResponse() const { return std::make_shared<MultiResponse>(); }


TestKeeper::TestKeeper(const String & root_path_, Poco::Timespan operation_timeout_)
    : root_path(root_path_), operation_timeout(operation_timeout_)
{
    container.emplace("/", Node());

    if (!root_path.empty())
    {
        if (root_path.back() == '/')
            root_path.pop_back();
    }

    processing_thread = ThreadFromGlobalPool([this] { processingThread(); });
}


TestKeeper::~TestKeeper()
{
    try
    {
        finalize();
        if (processing_thread.joinable())
            processing_thread.join();
    }
    catch (...)
    {
        tryLogCurrentException(__PRETTY_FUNCTION__);
    }
}


void TestKeeper::processingThread()
{
    setThreadName("TestKeeperProc");

    try
    {
        while (!expired)
        {
            RequestInfo info;

            UInt64 max_wait = UInt64(operation_timeout.totalMilliseconds());
            if (requests_queue.tryPop(info, max_wait))
            {
                if (expired)
                    break;

                if (info.watch)
                {
                    auto & watches_type = dynamic_cast<const ListRequest *>(info.request.get())
                        ? list_watches
                        : watches;

                    watches_type[info.request->getPath()].emplace_back(std::move(info.watch));
                }

                ++zxid;

                info.request->addRootPath(root_path);
                ResponsePtr response = info.request->process(container, zxid);
                if (response->error == Error::ZOK)
                    info.request->processWatches(watches, list_watches);

                response->removeRootPath(root_path);
                if (info.callback)
                    info.callback(*response);
            }
        }
    }
    catch (...)
    {
        tryLogCurrentException(__PRETTY_FUNCTION__);
        finalize();
    }
}


void TestKeeper::finalize()
{
    {
        std::lock_guard lock(push_request_mutex);

        if (expired)
            return;
        expired = true;
    }

    processing_thread.join();

    try
    {
        {
            for (auto & path_watch : watches)
            {
                WatchResponse response;
                response.type = SESSION;
                response.state = EXPIRED_SESSION;
                response.error = ZSESSIONEXPIRED;

                for (auto & callback : path_watch.second)
                {
                    if (callback)
                    {
                        try
                        {
                            callback(response);
                        }
                        catch (...)
                        {
                            tryLogCurrentException(__PRETTY_FUNCTION__);
                        }
                    }
                }
            }

            watches.clear();
        }

        RequestInfo info;
        while (requests_queue.tryPop(info))
        {
            if (info.callback)
            {
                ResponsePtr response = info.request->createResponse();
                response->error = ZSESSIONEXPIRED;
                try
                {
                    info.callback(*response);
                }
                catch (...)
                {
                    tryLogCurrentException(__PRETTY_FUNCTION__);
                }
            }
            if (info.watch)
            {
                WatchResponse response;
                response.type = SESSION;
                response.state = EXPIRED_SESSION;
                response.error = ZSESSIONEXPIRED;
                try
                {
                    info.watch(response);
                }
                catch (...)
                {
                    tryLogCurrentException(__PRETTY_FUNCTION__);
                }
            }
        }
    }
    catch (...)
    {
        tryLogCurrentException(__PRETTY_FUNCTION__);
    }
}

void TestKeeper::pushRequest(RequestInfo && info)
{
    try
    {
        info.time = clock::now();

        /// We must serialize 'pushRequest' and 'finalize' (from processingThread) calls
        ///  to avoid forgotten operations in the queue when session is expired.
        /// Invariant: when expired, no new operations will be pushed to the queue in 'pushRequest'
        ///  and the queue will be drained in 'finalize'.
        std::lock_guard lock(push_request_mutex);

        if (expired)
            throw Exception("Session expired", ZSESSIONEXPIRED);

        if (!requests_queue.tryPush(std::move(info), operation_timeout.totalMilliseconds()))
            throw Exception("Cannot push request to queue within operation timeout", ZOPERATIONTIMEOUT);
    }
    catch (...)
    {
        finalize();
        throw;
    }
}


void TestKeeper::create(
        const String & path,
        const String & data,
        bool is_ephemeral,
        bool is_sequential,
        const ACLs &,
        CreateCallback callback)
{
    TestKeeperCreateRequest request;
    request.path = path;
    request.data = data;
    request.is_ephemeral = is_ephemeral;
    request.is_sequential = is_sequential;

    RequestInfo request_info;
    request_info.request = std::make_shared<TestKeeperCreateRequest>(std::move(request));
    request_info.callback = [callback](const Response & response) { callback(dynamic_cast<const CreateResponse &>(response)); };
    pushRequest(std::move(request_info));
}

void TestKeeper::remove(
        const String & path,
        int32_t version,
        RemoveCallback callback)
{
    TestKeeperRemoveRequest request;
    request.path = path;
    request.version = version;

    RequestInfo request_info;
    request_info.request = std::make_shared<TestKeeperRemoveRequest>(std::move(request));
    request_info.callback = [callback](const Response & response) { callback(dynamic_cast<const RemoveResponse &>(response)); };
    pushRequest(std::move(request_info));
}

void TestKeeper::exists(
        const String & path,
        ExistsCallback callback,
        WatchCallback watch)
{
    TestKeeperExistsRequest request;
    request.path = path;

    RequestInfo request_info;
    request_info.request = std::make_shared<TestKeeperExistsRequest>(std::move(request));
    request_info.callback = [callback](const Response & response) { callback(dynamic_cast<const ExistsResponse &>(response)); };
    request_info.watch = watch;
    pushRequest(std::move(request_info));
}

void TestKeeper::get(
        const String & path,
        GetCallback callback,
        WatchCallback watch)
{
    TestKeeperGetRequest request;
    request.path = path;

    RequestInfo request_info;
    request_info.request = std::make_shared<TestKeeperGetRequest>(std::move(request));
    request_info.callback = [callback](const Response & response) { callback(dynamic_cast<const GetResponse &>(response)); };
    request_info.watch = watch;
    pushRequest(std::move(request_info));
}

void TestKeeper::set(
        const String & path,
        const String & data,
        int32_t version,
        SetCallback callback)
{
    TestKeeperSetRequest request;
    request.path = path;
    request.data = data;
    request.version = version;

    RequestInfo request_info;
    request_info.request = std::make_shared<TestKeeperSetRequest>(std::move(request));
    request_info.callback = [callback](const Response & response) { callback(dynamic_cast<const SetResponse &>(response)); };
    pushRequest(std::move(request_info));
}

void TestKeeper::list(
        const String & path,
        ListCallback callback,
        WatchCallback watch)
{
    TestKeeperListRequest request;
    request.path = path;

    RequestInfo request_info;
    request_info.request = std::make_shared<TestKeeperListRequest>(std::move(request));
    request_info.callback = [callback](const Response & response) { callback(dynamic_cast<const ListResponse &>(response)); };
    request_info.watch = watch;
    pushRequest(std::move(request_info));
}

void TestKeeper::check(
        const String & path,
        int32_t version,
        CheckCallback callback)
{
    TestKeeperCheckRequest request;
    request.path = path;
    request.version = version;

    RequestInfo request_info;
    request_info.request = std::make_shared<TestKeeperCheckRequest>(std::move(request));
    request_info.callback = [callback](const Response & response) { callback(dynamic_cast<const CheckResponse &>(response)); };
    pushRequest(std::move(request_info));
}

void TestKeeper::multi(
        const Requests & requests,
        MultiCallback callback)
{
    TestKeeperMultiRequest request(requests);

    RequestInfo request_info;
    request_info.request = std::make_shared<TestKeeperMultiRequest>(std::move(request));
    request_info.callback = [callback](const Response & response) { callback(dynamic_cast<const MultiResponse &>(response)); };
    pushRequest(std::move(request_info));
}

}
