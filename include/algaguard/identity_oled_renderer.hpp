#pragma once
#include <array>
#include "algaguard/identity_status_view_model.hpp"
namespace algaguard::host_test {
enum class IdentityScreenMode { MAIN_STATUS, SECURITY_DIAGNOSTICS };
struct OledRenderLine { unsigned row{}; std::string text; };
struct IdentityOledRender { static constexpr unsigned width=128,height=64,max_chars=21; std::array<OledRenderLine,4> lines; unsigned count{}; bool warningVisible{}; bool truncated{}; };
class IdentityOledRenderer { public: IdentityOledRender render(const IdentityStatusViewModel& v, IdentityScreenMode mode) const { IdentityOledRender r; add(r,0,v.primaryText);add(r,1,v.secondaryText);if(v.warningPersistent&&!v.warningText.empty()){add(r,2,v.warningText);r.warningVisible=true;}if(mode==IdentityScreenMode::SECURITY_DIAGNOSTICS)add(r,3,v.diagnosticsText);return r;} private:void add(IdentityOledRender& r,unsigned row,const std::string& text) const {if(r.count==4)return;auto safe=redactSecretLike(text);auto clipped=truncateSafe(safe,IdentityOledRender::max_chars);r.truncated|=clipped.size()!=safe.size();r.lines[r.count++]={row,clipped};}};
}
