#include "MainFrame.h"

#include <wx/app.h>

namespace factoria
{
class FactorIAApp final : public wxApp
{
public:
    bool OnInit() override
    {
        if (!wxApp::OnInit())
            return false;
        auto* frame = new MainFrame();
        frame->Show();
        return true;
    }
};
}

wxIMPLEMENT_APP(factoria::FactorIAApp);

