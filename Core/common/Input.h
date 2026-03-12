#ifndef PQC_MASTER_THESIS_2026_INPUT_H
#define PQC_MASTER_THESIS_2026_INPUT_H

#include "KeyCodes.h"
#include <glm/glm.hpp>

namespace Safira {

    class Input {
    public:
        static bool IsKeyDown(KeyCode keycode);
        static bool IsMouseButtonDown(MouseButton button);

        static glm::vec2 GetMousePosition();

        static void SetCursorMode(CursorMode mode);
    };

}

#endif //PQC_MASTER_THESIS_2026_INPUT_H