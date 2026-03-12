#ifndef PQC_MASTER_THESIS_2026_LAYER_H
#define PQC_MASTER_THESIS_2026_LAYER_H

namespace Safira {
    class Layer {
    public:
        virtual ~Layer() = default;

        virtual void OnAttach() {}
        virtual void OnDetach() {}
        virtual void OnUpdate(float timestep) {}
        virtual void OnUIRender() {}
    };
}

#endif //PQC_MASTER_THESIS_2026_LAYER_H