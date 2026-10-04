#include <romfs/romfs.hpp>

#include <algorithm>
#include <cstdlib>
#include <fstream>
#include <iterator>
#include <map>
#include <mutex>
#include <stdexcept>
#include <string>
#include <system_error>
#include <vector>

const std::map<fs::path, romfs::Resource>& ROMFS_CONCAT(ROMFS_NAME, _get_resources)();
const std::vector<fs::path>& ROMFS_CONCAT(ROMFS_NAME, _get_paths)();
const std::string& ROMFS_CONCAT(ROMFS_NAME, _get_name)();

#if defined(PS5)
/* native_fs.c 通过 open(O_DIRECTORY)+getdents 枚举；标题沙箱里的 opendir() 对资源目录会
 * 返回 EPERM，因此 PS5 的 list() 不能使用 fs::directory_iterator。 */
extern "C" int wiliwili_list_dir(const char *path, char names[][256], int maxNames);
#endif

namespace {

    /*
     * 资源覆盖层：资源树随包以松散文件发布到 /app0/assets/，romfs::get/list 于是
     * "文件优先、内嵌回退"。覆盖层文件一旦读入就驻留在静态存储里，因为返回的
     * Resource 会以引用形式被调用方长期持有（例如常驻的字体、图标）。
     *
     * std::map 是节点式容器：插入新键不会移动已有节点，所以 contents 里的 string
     * 地址稳定，resources 里指向它的 span/Resource 也就一直有效。两个 map 共用同一
     * 把锁（读取与插入一起串行化；首帧加载后命中的都是缓存，不碰磁盘）。
     */
    struct OverlayStore {
        std::mutex mutex;
        std::map<fs::path, std::string> contents;      /* 文件字节，末尾补一个 NUL */
        std::map<fs::path, romfs::Resource> resources; /* span 指向 contents 中的 string */
    };

    OverlayStore &overlay_store() {
        static OverlayStore store;
        return store;
    }

    /* 覆盖层根目录：WILIWILI_RES_DIR 优先，其次 PS5 上随包安装的 /app0/assets/。
     * 其它平台默认不启用（保持上游行为）；显式设置环境变量时仍可用，方便桌面端
     * 验证同一套覆盖逻辑。 */
    const char *overlay_root() {
        const char *from_environment = std::getenv("WILIWILI_RES_DIR");
        if (from_environment != nullptr && from_environment[0] != '\0') return from_environment;
#if defined(PS5)
        return "/app0/assets/";
#else
        return nullptr;
#endif
    }

    /* 列出一层目录的条目名；PS5 走 getdents 包装，其它平台走 directory_iterator。 */
    std::vector<std::string> overlay_entries(const std::string &directory) {
        std::vector<std::string> names;
#if defined(PS5)
        const int max_names = 512;
        std::vector<char> buffer(static_cast<std::size_t>(max_names) * 256);
        const int count =
            wiliwili_list_dir(directory.c_str(), reinterpret_cast<char (*)[256]>(buffer.data()), max_names);
        if (count <= 0) return names;
        for (int i = 0; i < count; ++i) names.emplace_back(buffer.data() + static_cast<std::size_t>(i) * 256);
#else
#if defined(USE_BOOST_FILESYSTEM)
        boost::system::error_code ignored;
#else
        std::error_code ignored;
#endif
        for (const auto &entry : fs::directory_iterator(directory, ignored))
            names.push_back(entry.path().filename().string());
#endif
        return names;
    }

} // namespace

namespace romfs {

    const romfs::Resource &impl::ROMFS_CONCAT(get_, LIBROMFS_PROJECT_NAME)(const fs::path &path) {
        if (const char *root = overlay_root()) {
            OverlayStore &store = overlay_store();
            std::lock_guard<std::mutex> lock(store.mutex);

            const auto cached = store.resources.find(path);
            if (cached != store.resources.end()) return cached->second;

            std::ifstream file((fs::path(root) / path).string(), std::ios::binary);
            if (file) {
                std::string content((std::istreambuf_iterator<char>(file)), std::istreambuf_iterator<char>());
                /* 空文件回退而不是当作命中：一个 0 字节文件会把资源悄悄吃掉，
                 * 症状是"加载出一个空串"，而根因（复制或打包出错）不会留下任何
                 * 提示 —— 回退到内嵌副本至少能让标题继续跑。 */
                if (!content.empty()) {
                    /* 与内嵌资源格式对齐：生成器在字节数组末尾补一个 0x00，span 长度
                     * 为文件长度。这里同样补 NUL，让 Resource::string() 的 size()+1
                     * 视图安全。 */
                    content.push_back('\0');
                    std::string &stored = store.contents.try_emplace(path, std::move(content)).first->second;
                    romfs::Resource &resource = store.resources
                                                    .try_emplace(path,
                                                                 nonstd::span<std::byte>(
                                                                     reinterpret_cast<std::byte *>(stored.data()),
                                                                     stored.size() - 1))
                                                    .first->second;
                    /* 节点地址稳定：锁释放后这个引用依然有效。 */
                    return resource;
                }
            }
        }

        try {
            return ROMFS_CONCAT(ROMFS_NAME, _get_resources)().at(path);
        } catch (const std::out_of_range &) {
            throw std::invalid_argument(
                std::string("Invalid romfs resource path for '" + romfs::name() + "' : ") + path.string());
        }
    }

    std::vector<fs::path> impl::ROMFS_CONCAT(list_, LIBROMFS_PROJECT_NAME)(const fs::path &parent) {
        std::vector<fs::path> result;
        if (parent.empty()) {
            result = ROMFS_CONCAT(ROMFS_NAME, _get_paths)();
        } else {
            for (const auto &p : ROMFS_CONCAT(ROMFS_NAME, _get_paths)())
                if (p.parent_path() == parent)
                    result.push_back(p);
        }

        /* 外置资源可能不在内嵌归档里（这正是外置的意义），所以把文件系统里该
         * parent 下的直接子项并进来，并按路径去重：内嵌拷贝优先，语义仍是"直接
         * 子项"（i18n.cpp 用 list("i18n/<locale>") 找 .json）。 */
        if (const char *root = overlay_root()) {
            const fs::path directory = fs::path(root) / parent;
            for (const auto &name : overlay_entries(directory.string())) {
                fs::path candidate = parent / name;
                if (std::find(result.begin(), result.end(), candidate) == result.end())
                    result.push_back(candidate);
            }
        }

        return result;
    }

    const std::string &impl::ROMFS_CONCAT(name_, LIBROMFS_PROJECT_NAME)() {
        return ROMFS_CONCAT(ROMFS_NAME, _get_name)();
    }

}
