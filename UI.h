#pragma once

// Optional in-game settings menu, drawn through SKSE Menu Framework. The
// framework is reached by GetProcAddress only, so there is nothing to link
// against and nothing to install for the plugin to work.
namespace UI {
    // Registers the pages, or does nothing if the framework is absent.
    void Register();

    namespace General {
        void __stdcall Render();
    }

    namespace Categories {
        void __stdcall Render();
    }

    namespace Folders {
        void __stdcall Render();
    }

    namespace Browse {
        void __stdcall Render();
    }
}
