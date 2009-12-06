#ifndef __MODEL_H__
#define __MODEL_H__

#include <vectormath/cpp/vectormath_aos.h>
#include <ddf/ddf.h>
#include "render/mesh_ddf.h"
#include "../rendercontext.h"
#include "../material.h"


namespace dmModel
{
    using namespace Vectormath::Aos;


    typedef struct SModel* HModel;
    typedef class ModelWorld* HWorld;
    typedef void (*SetObjectModel)(void* context, void* gameobject, Quat* rotation, Point3* position);


    /**
     * Create a new model world
     * @param max_models number of models in world
     * @param set_object_model callback fn to update model orientation from gameobject
     * @return new model world handle
     */
    HWorld NewWorld(uint32_t max_models, SetObjectModel set_object_model);

    /**
     * Destroy model world
     * @param handle to world to destroy
     */
    void DeleteWorld(HWorld);

    /**
     * Update model world context
     * @param world model world
     * @param rendercontext rendercontext to use
     */
    void UpdateContext(HWorld world, RenderContext* rendercontext);

    /**
     * Render a world
     * @param world model world
     */
    void RenderWorld(HWorld world);

    /**
     * Create a new model
     * @return new model handle
     */
    HModel NewModel();

    /**
     * Create a new model with parameters
     * @param prototype prototype model
     * @param gameobject game object
     * @param collection game object collection
     * @return newly created model
     */
    HModel NewModel(HModel prototype, void* gameobject, void* collection);

    /**
     * Destroy a model
     * @param model model to destroy
     */
    void DeleteModel(HModel model);

    /**
     * Set model mesh
     * @param model model
     * @param mesh mesh
     */
    void SetMesh(HModel model, Render::Mesh* mesh);

    /**
     * Set model texture0
     * @param model model
     * @param texture texture0
     */
    void SetTexture0(HModel model, dmGraphics::HTexture texture);

    /**
     * Set model material
     * @param model model
     * @param material material
     */
    void SetMaterial(HModel model, SMaterial* material);

    /**
     * Get mesh from model
     * @param model Model
     * @return Mesh associated with model
     */
    Render::Mesh* GetMesh(HModel model);

    /**
     * Get texture0 from model
     * @param model Model
     * @return Texture0 associated with model
     */
    dmGraphics::HTexture GetTexture0(HModel model);

    /**
     * Get material from model
     * @param model Model
     * @return Material associated with model
     */
    SMaterial* GetMaterial(HModel model);

    /**
     * Add model to world
     * @param world model world
     * @param model model
     */
    void AddModel(HWorld world, HModel model);

}


#endif //__MODEL_H__
