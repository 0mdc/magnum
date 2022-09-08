#ifndef Magnum_SceneTools_Implementation_sceneConverterUtilities_h
#define Magnum_SceneTools_Implementation_sceneConverterUtilities_h
/*
    This file is part of Magnum.

    Copyright © 2010, 2011, 2012, 2013, 2014, 2015, 2016, 2017, 2018, 2019,
                2020, 2021, 2022 Vladimír Vondruš <mosra@centrum.cz>

    Permission is hereby granted, free of charge, to any person obtaining a
    copy of this software and associated documentation files (the "Software"),
    to deal in the Software without restriction, including without limitation
    the rights to use, copy, modify, merge, publish, distribute, sublicense,
    and/or sell copies of the Software, and to permit persons to whom the
    Software is furnished to do so, subject to the following conditions:

    The above copyright notice and this permission notice shall be included
    in all copies or substantial portions of the Software.

    THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
    IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
    FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. IN NO EVENT SHALL
    THE AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
    LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING
    FROM, OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER
    DEALINGS IN THE SOFTWARE.
*/

#include <cctype> /* std::isupper() */
#include <unordered_map> /* sceneFieldNames */
#include <Corrade/Containers/Optional.h>
#include <Corrade/Containers/Pair.h>
#include <Corrade/Utility/Arguments.h>

#include "Magnum/Math/FunctionsBatch.h"
#include "Magnum/Trade/AnimationData.h"
#include "Magnum/Trade/CameraData.h"
#include "Magnum/Trade/LightData.h"
#include "Magnum/Trade/MaterialData.h"
#include "Magnum/Trade/MeshData.h"
#include "Magnum/Trade/SceneData.h"
#include "Magnum/Trade/SkinData.h"
#include "Magnum/Trade/TextureData.h"

#include "Magnum/Trade/Implementation/converterUtilities.h"

namespace Magnum { namespace SceneTools { namespace Implementation {

using namespace Containers::Literals;

/* Used only in executables where we don't want it to be exported -- in
   particular magnum-sceneconverter and its tests */
namespace {

/** @todo const Array& doesn't work, minmax() would fail to match */
template<class T> Containers::String calculateBounds(Containers::Array<T>&& attribute) {
    /** @todo clean up when Debug::toString() exists */
    std::ostringstream out;
    Debug{&out, Debug::Flag::NoNewlineAtTheEnd} << Debug::packed << Math::minmax(attribute);
    return out.str();
}

/* Named attribute index from a global index */
/** @todo some helper for this directly on the MeshData class? */
UnsignedInt namedAttributeId(const Trade::MeshData& mesh, UnsignedInt id) {
    const Trade::MeshAttribute name = mesh.attributeName(id);
    for(UnsignedInt i = 0; i != mesh.attributeCount(name); ++i)
        if(mesh.attributeId(name, i) == id) return i;
    CORRADE_INTERNAL_ASSERT_UNREACHABLE(); /* LCOV_EXCL_LINE */
}

bool printInfo(const Debug::Flags useColor, const bool useColor24, const Utility::Arguments& args, Trade::AbstractImporter& importer, std::chrono::high_resolution_clock::duration& importTime) {
    struct AnimationInfo {
        UnsignedInt animation;
        Trade::AnimationData data{{}, {}};
        Containers::String name;
    };

    struct SkinInfo {
        bool twoDimensions;
        UnsignedInt skin;
        UnsignedInt jointCount;
        Containers::String name;
    };

    struct LightInfo {
        UnsignedInt light;
        Trade::LightData data{{}, {}, {}};
        Containers::String name;
    };

    struct CameraInfo {
        UnsignedInt camera;
        Trade::CameraData data{{}, {}, {}, {}};
        Containers::String name;
    };

    struct MaterialInfo {
        UnsignedInt material;
        Trade::MaterialData data{{}, {}};
        Containers::String name;
    };

    struct TextureInfo {
        UnsignedInt texture;
        Trade::TextureData data{{}, {}, {}, {}, {}, {}};
        Containers::String name;
    };

    struct MeshAttributeInfo {
        std::size_t offset;
        Int stride;
        UnsignedInt arraySize;
        Trade::MeshAttribute name;
        Containers::String customName;
        VertexFormat format;
        Containers::String bounds;
    };

    struct MeshInfo {
        UnsignedInt mesh, level;
        MeshPrimitive primitive;
        UnsignedInt indexCount, vertexCount;
        std::size_t indexOffset;
        Int indexStride;
        Containers::String indexBounds;
        MeshIndexType indexType;
        Containers::Array<MeshAttributeInfo> attributes;
        std::size_t indexDataSize, vertexDataSize;
        Trade::DataFlags indexDataFlags, vertexDataFlags;
        Containers::String name;
    };

    struct SceneFieldInfo {
        Trade::SceneField name;
        Trade::SceneFieldFlags flags;
        Trade::SceneFieldType type;
        UnsignedInt arraySize;
        std::size_t size;
    };

    struct SceneInfo {
        UnsignedInt scene;
        Trade::SceneMappingType mappingType;
        UnsignedLong mappingBound;
        Containers::Array<SceneFieldInfo> fields;
        std::size_t dataSize;
        Trade::DataFlags dataFlags;
        Containers::String name;
    };

    struct ObjectInfo {
        UnsignedLong object;
        /* A bitfield, assuming no more than 32 scenes */
        /** @todo might be too little? */
        UnsignedInt scenes;
        Containers::Array<Containers::Pair<Trade::SceneField, UnsignedInt>> fields;
        Containers::String name;
    };

    /* Parse everything first to avoid errors interleaved with output */
    bool error = false;

    /* Object properties */
    Containers::Array<ObjectInfo> objectInfos;
    if(args.isSet("info") || args.isSet("info-objects")) {
        objectInfos = Containers::Array<ObjectInfo>{std::size_t(importer.objectCount())};

        for(UnsignedLong i = 0; i != importer.objectCount(); ++i) {
            objectInfos[i].object = i;
            objectInfos[i].name = importer.objectName(i);
        }
    }

    /* Scene properties, together with counting how much is each mesh / light /
       material / skin / object referenced (which gets used only if both
       --info-scenes and --info-{lights,materials,skins,objects} is passed and
       the file has at least one scene). Texture reference count is calculated
       when parsing materials. */
    Containers::Array<SceneInfo> sceneInfos;
    /* Only the very latest GCC seems to support enum classes as keys and I
       can't be bothered to write a std::hash specialization, so just making
       the key typeless */
    std::unordered_map<UnsignedInt, Containers::String> sceneFieldNames;
    Containers::Array<UnsignedInt> materialReferenceCount;
    Containers::Array<UnsignedInt> lightReferenceCount;
    Containers::Array<UnsignedInt> cameraReferenceCount;
    Containers::Array<UnsignedInt> meshReferenceCount;
    Containers::Array<UnsignedInt> skin2DReferenceCount;
    Containers::Array<UnsignedInt> skin3DReferenceCount;
    if((args.isSet("info") || args.isSet("info-scenes")) && importer.sceneCount()) {
        materialReferenceCount = Containers::Array<UnsignedInt>{importer.materialCount()};
        lightReferenceCount = Containers::Array<UnsignedInt>{importer.lightCount()};
        cameraReferenceCount = Containers::Array<UnsignedInt>{importer.cameraCount()};
        meshReferenceCount = Containers::Array<UnsignedInt>{importer.meshCount()};
        skin2DReferenceCount = Containers::Array<UnsignedInt>{importer.skin2DCount()};
        skin3DReferenceCount = Containers::Array<UnsignedInt>{importer.skin3DCount()};

        for(UnsignedInt i = 0; i != importer.sceneCount(); ++i) {
            Containers::Optional<Trade::SceneData> scene = importer.scene(i);
            if(!scene) {
                error = true;
                continue;
            }

            SceneInfo info{};
            info.scene = i;
            info.mappingType = scene->mappingType();
            info.mappingBound = scene->mappingBound();
            info.dataSize = scene->data().size();
            info.dataFlags = scene->dataFlags();
            info.name = importer.sceneName(i);
            for(UnsignedInt j = 0; j != scene->fieldCount(); ++j) {
                const Trade::SceneField name = scene->fieldName(j);

                if(name == Trade::SceneField::Mesh) for(const Containers::Pair<UnsignedInt, Containers::Pair<UnsignedInt, Int>>& meshMaterial: scene->meshesMaterialsAsArray()) {
                    if(meshMaterial.second().first() < meshReferenceCount.size())
                        ++meshReferenceCount[meshMaterial.second().first()];
                    if(UnsignedInt(meshMaterial.second().second()) < materialReferenceCount.size())
                        ++materialReferenceCount[meshMaterial.second().second()];
                }

                if(name == Trade::SceneField::Skin) for(const Containers::Pair<UnsignedInt, UnsignedInt> skin: scene->skinsAsArray()) {
                    if(scene->is2D() && skin.second() < skin2DReferenceCount.size())
                        ++skin2DReferenceCount[skin.second()];
                    if(scene->is3D() && skin.second() < skin3DReferenceCount.size())
                        ++skin3DReferenceCount[skin.second()];
                }

                if(name == Trade::SceneField::Light) for(const Containers::Pair<UnsignedInt, UnsignedInt>& light: scene->lightsAsArray()) {
                    if(light.second() < lightReferenceCount.size())
                        ++lightReferenceCount[light.second()];
                }

                if(name == Trade::SceneField::Camera) for(const Containers::Pair<UnsignedInt, UnsignedInt>& camera: scene->camerasAsArray()) {
                    if(camera.second() < cameraReferenceCount.size())
                        ++cameraReferenceCount[camera.second()];
                }

                arrayAppend(info.fields, InPlaceInit,
                    name,
                    scene->fieldFlags(j),
                    scene->fieldType(j),
                    scene->fieldArraySize(j),
                    scene->fieldSize(j));

                /* If the field has a custom name, save it into the map. Not
                   putting it into the fields array as the map is reused by
                   object info as well. */
                if(Trade::isSceneFieldCustom(name)) {
                    /* Fetch the name only if it's not already there */
                    const auto inserted = sceneFieldNames.emplace(sceneFieldCustom(name), Containers::String{});
                    if(inserted.second)
                        inserted.first->second = importer.sceneFieldName(name);
                }

                if(objectInfos) for(const UnsignedInt object: scene->mappingAsArray(j)) {
                    if(object >= objectInfos.size()) continue;

                    objectInfos[object].object = object;
                    objectInfos[object].scenes |= 1 << i;

                    /* If the field is repeated, increase the count instead */
                    if(!objectInfos[object].fields.isEmpty() && objectInfos[object].fields.back().first() == name)
                        ++objectInfos[object].fields.back().second();
                    else
                        arrayAppend(objectInfos[object].fields, InPlaceInit, name, 1u);
                }
            }

            arrayAppend(sceneInfos, std::move(info));
        }
    }

    /* Animation properties */
    Containers::Array<AnimationInfo> animationInfos;
    if(args.isSet("info") || args.isSet("info-animations")) for(UnsignedInt i = 0; i != importer.animationCount(); ++i) {
        Containers::Optional<Trade::AnimationData> animation;
        {
            Trade::Implementation::Duration d{importTime};
            if(!(animation = importer.animation(i))) {
                error = true;
                continue;
            }
        }

        AnimationInfo info{};
        info.animation = i;
        info.name = importer.animationName(i);
        info.data = *std::move(animation);

        arrayAppend(animationInfos, std::move(info));
    }

    /* Skin properties */
    Containers::Array<SkinInfo> skinInfos;
    if(args.isSet("info") || args.isSet("info-skins")) {
        for(UnsignedInt i = 0; i != importer.skin2DCount(); ++i) {
            Containers::Optional<Trade::SkinData2D> skin;
            {
                Trade::Implementation::Duration d{importTime};
                if(!(skin = importer.skin2D(i))) {
                    error = true;
                    continue;
                }
            }

            SkinInfo info{};
            info.twoDimensions = true;
            info.skin = i;
            info.name = importer.skin2DName(i);
            info.jointCount = skin->joints().size();

            arrayAppend(skinInfos, std::move(info));
        }

        for(UnsignedInt i = 0; i != importer.skin3DCount(); ++i) {
            Containers::Optional<Trade::SkinData3D> skin;
            {
                Trade::Implementation::Duration d{importTime};
                if(!(skin = importer.skin3D(i))) {
                    error = true;
                    continue;
                }
            }

            SkinInfo info{};
            info.twoDimensions = false;
            info.skin = i;
            info.name = importer.skin3DName(i);
            info.jointCount = skin->joints().size();

            arrayAppend(skinInfos, std::move(info));
        }
    }

    /* Light properties */
    Containers::Array<LightInfo> lightInfos;
    if(args.isSet("info") || args.isSet("info-lights")) for(UnsignedInt i = 0; i != importer.lightCount(); ++i) {
        Containers::Optional<Trade::LightData> light;
        {
            Trade::Implementation::Duration d{importTime};
            if(!(light = importer.light(i))) {
                error = true;
                continue;
            }
        }

        LightInfo info{};
        info.light = i;
        info.name = importer.lightName(i);
        info.data = *std::move(light);

        arrayAppend(lightInfos, std::move(info));
    }

    /* Camera properties */
    Containers::Array<CameraInfo> cameraInfos;
    if(args.isSet("info") || args.isSet("info-cameras")) for(UnsignedInt i = 0; i != importer.cameraCount(); ++i) {
        Containers::Optional<Trade::CameraData> camera;
        {
            Trade::Implementation::Duration d{importTime};
            if(!(camera = importer.camera(i))) {
                error = true;
                continue;
            }
        }

        CameraInfo info{};
        info.camera = i;
        info.name = importer.cameraName(i);
        info.data = *std::move(camera);

        arrayAppend(cameraInfos, std::move(info));
    }

    /* Material properties, together with how much is each texture shared
       (which gets used only if both --info-materials and --info-textures is
       passed and the file has at least one material). */
    Containers::Array<MaterialInfo> materialInfos;
    Containers::Array<UnsignedInt> textureReferenceCount;
    if((args.isSet("info") || args.isSet("info-materials")) && importer.materialCount()) {
        textureReferenceCount = Containers::Array<UnsignedInt>{importer.textureCount()};

        for(UnsignedInt i = 0; i != importer.materialCount(); ++i) {
            Containers::Optional<Trade::MaterialData> material;
            {
                Trade::Implementation::Duration d{importTime};
                if(!(material = importer.material(i))) {
                    error = true;
                    continue;
                }
            }

            /* Calculate texture reference count for all properties that look
               like a texture */
            for(UnsignedInt j = 0; j != material->layerCount(); ++j) {
                for(UnsignedInt k = 0; k != material->attributeCount(j); ++k) {
                    if(material->attributeType(j, k) != Trade::MaterialAttributeType::UnsignedInt || !material->attributeName(j, k).hasSuffix("Texture"_s))
                        continue;

                    const UnsignedInt texture = material->attribute<UnsignedInt>(j, k);
                    /** @todo once StridedBitArrayView2D exists, fix this to
                        count each material only once by having one bit for
                        every material and texture */
                    if(texture < textureReferenceCount.size())
                        ++textureReferenceCount[texture];
                }
            }

            MaterialInfo info{};
            info.material = i;
            info.name = importer.materialName(i);
            info.data = *std::move(material);

            arrayAppend(materialInfos, std::move(info));
        }
    }

    /* Mesh properties */
    Containers::Array<MeshInfo> meshInfos;
    if(args.isSet("info") || args.isSet("info-meshes")) for(UnsignedInt i = 0; i != importer.meshCount(); ++i) {
        for(UnsignedInt j = 0; j != importer.meshLevelCount(i); ++j) {
            Containers::Optional<Trade::MeshData> mesh;
            {
                Trade::Implementation::Duration d{importTime};
                if(!(mesh = importer.mesh(i, j))) {
                    error = true;
                    continue;
                }
            }

            MeshInfo info{};
            info.mesh = i;
            info.level = j;
            info.primitive = mesh->primitive();
            info.vertexCount = mesh->vertexCount();
            info.vertexDataSize = mesh->vertexData().size();
            info.vertexDataFlags = mesh->vertexDataFlags();
            if(!j) {
                info.name = importer.meshName(i);
            }
            if(mesh->isIndexed()) {
                info.indexCount = mesh->indexCount();
                info.indexType = mesh->indexType();
                info.indexOffset = mesh->indexOffset();
                info.indexStride = mesh->indexStride();
                info.indexDataSize = mesh->indexData().size();
                info.indexDataFlags = mesh->indexDataFlags();
                if(args.isSet("bounds"))
                    info.indexBounds = calculateBounds(mesh->indicesAsArray());
            }
            for(UnsignedInt k = 0; k != mesh->attributeCount(); ++k) {
                const Trade::MeshAttribute name = mesh->attributeName(k);

                /* Calculate bounds, if requested, if this is not an
                   implementation-specific format and if it's not a custom
                   attribute */
                Containers::String bounds;
                if(args.isSet("bounds") && !isVertexFormatImplementationSpecific(mesh->attributeFormat(k))) switch(name) {
                    case Trade::MeshAttribute::Position:
                        bounds = calculateBounds(mesh->positions3DAsArray(namedAttributeId(*mesh, k)));
                        break;
                    case Trade::MeshAttribute::Tangent:
                        bounds = calculateBounds(mesh->tangentsAsArray(namedAttributeId(*mesh, k)));
                        break;
                    case Trade::MeshAttribute::Bitangent:
                        bounds = calculateBounds(mesh->bitangentsAsArray(namedAttributeId(*mesh, k)));
                        break;
                    case Trade::MeshAttribute::Normal:
                        bounds = calculateBounds(mesh->normalsAsArray(namedAttributeId(*mesh, k)));
                        break;
                    case Trade::MeshAttribute::TextureCoordinates:
                        bounds = calculateBounds(mesh->textureCoordinates2DAsArray(namedAttributeId(*mesh, k)));
                        break;
                    case Trade::MeshAttribute::Color:
                        bounds = calculateBounds(mesh->colorsAsArray(namedAttributeId(*mesh, k)));
                        break;
                    case Trade::MeshAttribute::ObjectId:
                        bounds = calculateBounds(mesh->objectIdsAsArray(namedAttributeId(*mesh, k)));
                        break;
                }

                arrayAppend(info.attributes, InPlaceInit,
                    mesh->attributeOffset(k),
                    mesh->attributeStride(k),
                    mesh->attributeArraySize(k),
                    name, Trade::isMeshAttributeCustom(name) ?
                        importer.meshAttributeName(name) : "",
                    mesh->attributeFormat(k),
                    bounds);
            }

            arrayAppend(meshInfos, std::move(info));
        }
    }

    /* Texture properties, together with how much is each image shared (which
       gets used only if both --info-textures and --info-images is passed and
       the file has at least one texture). */
    Containers::Array<TextureInfo> textureInfos;
    Containers::Array<UnsignedInt> image1DReferenceCount;
    Containers::Array<UnsignedInt> image2DReferenceCount;
    Containers::Array<UnsignedInt> image3DReferenceCount;
    if((args.isSet("info") || args.isSet("info-textures")) && importer.textureCount()) {
        image1DReferenceCount = Containers::Array<UnsignedInt>{importer.image1DCount()};
        image2DReferenceCount = Containers::Array<UnsignedInt>{importer.image2DCount()};
        image3DReferenceCount = Containers::Array<UnsignedInt>{importer.image3DCount()};
        for(UnsignedInt i = 0; i != importer.textureCount(); ++i) {
            Containers::Optional<Trade::TextureData> texture;
            {
                Trade::Implementation::Duration d{importTime};
                if(!(texture = importer.texture(i))) {
                    error = true;
                    continue;
                }
            }

            switch(texture->type()) {
                case Trade::TextureType::Texture1D:
                    if(texture->image() < image1DReferenceCount.size())
                        ++image1DReferenceCount[texture->image()];
                    break;
                case Trade::TextureType::Texture1DArray:
                case Trade::TextureType::Texture2D:
                    if(texture->image() < image2DReferenceCount.size())
                        ++image2DReferenceCount[texture->image()];
                    break;
                case Trade::TextureType::CubeMap:
                case Trade::TextureType::CubeMapArray:
                case Trade::TextureType::Texture2DArray:
                case Trade::TextureType::Texture3D:
                    if(texture->image() < image3DReferenceCount.size())
                        ++image3DReferenceCount[texture->image()];
                    break;
            }

            TextureInfo info{};
            info.texture = i;
            info.name = importer.textureName(i);
            info.data = *std::move(texture);

            arrayAppend(textureInfos, std::move(info));
        }
    }

    Containers::Array<Trade::Implementation::ImageInfo> imageInfos;
    if(args.isSet("info") || args.isSet("info-images")) {
        imageInfos = Trade::Implementation::imageInfo(importer, error, importTime);
    }

    std::size_t totalSceneDataSize = 0;
    for(const SceneInfo& info: sceneInfos) {
        Debug d{useColor};
        d << Debug::boldColor(Debug::Color::Default) << "Scene" << info.scene << Debug::nospace << ":" << Debug::resetColor;
        if(info.name) d << Debug::boldColor(Debug::Color::Yellow) << info.name << Debug::resetColor;
        d << Debug::newline;
        d << "  Bound:" << info.mappingBound << "objects"
            << Debug::color(Debug::Color::Blue) << "@" << Debug::packed
            << Debug::color(Debug::Color::Cyan) << info.mappingType
            << Debug::resetColor << "(" << Debug::nospace
            << Utility::format("{:.1f}", info.dataSize/1024.0f) << "kB";
        if(info.dataFlags != (Trade::DataFlag::Owned|Trade::DataFlag::Mutable))
            d << Debug::nospace << "," << Debug::packed
                << Debug::color(Debug::Color::Green) << info.dataFlags
                << Debug::resetColor;
        d << Debug::nospace << ")";

        d << Debug::newline << "  Fields:";
        for(const SceneFieldInfo& field: info.fields) {
            d << Debug::newline << "   "
                << Debug::boldColor(Debug::Color::Default);
            if(Trade::isSceneFieldCustom(field.name)) {
                d << "Custom(" << Debug::nospace
                    << Trade::sceneFieldCustom(field.name)
                    << Debug::nospace << ":" << Debug::nospace
                    << Debug::color(Debug::Color::Yellow)
                    << sceneFieldNames[sceneFieldCustom(field.name)]
                    << Debug::nospace
                    << Debug::boldColor(Debug::Color::Default) << ")";
            } else d << Debug::packed << field.name;

            d << Debug::color(Debug::Color::Blue) << "@" << Debug::packed << Debug::color(Debug::Color::Cyan) << field.type;
            if(field.arraySize)
                d << Debug::nospace << Utility::format("[{}]", field.arraySize);
            d << Debug::resetColor;
            if(field.flags) d << Debug::nospace << ","
                << Debug::packed << Debug::color(Debug::Color::Green)
                << field.flags << Debug::resetColor;
            d << Debug::nospace << "," << field.size << "entries";
        }

        totalSceneDataSize += info.dataSize;
    }
    if(!sceneInfos.isEmpty())
        Debug{} << "Total scene data size:" << Utility::format("{:.1f}", totalSceneDataSize/1024.0f) << "kB";

    for(const ObjectInfo& info: objectInfos) {
        /* Objects without a name and not referenced by any scenes are useless,
           ignore */
        if(!info.name && !info.scenes) continue;

        Debug d{useColor};
        d << Debug::boldColor(Debug::Color::Default) << "Object" << info.object << Debug::resetColor;

        if(sceneInfos) {
            const UnsignedInt count = Math::popcount(info.scenes);
            if(!count) d << Debug::color(Debug::Color::Red);
            d << "(referenced by" << count << "scenes)";
            if(!count) d << Debug::resetColor;
        }

        d << Debug::boldColor(Debug::Color::Default) << Debug::nospace << ":"
            << Debug::resetColor;
        if(info.name) d << Debug::boldColor(Debug::Color::Yellow)
            << info.name << Debug::resetColor;
        if(info.scenes) {
            d << Debug::newline << "  Fields:";

            for(std::size_t i = 0; i != info.fields.size(); ++i) {
                if(i) d << Debug::nospace << ",";
                const Containers::Pair<Trade::SceneField, UnsignedInt> nameCount = info.fields[i];
                d << Debug::color(Debug::Color::Cyan);
                if(Trade::isSceneFieldCustom(nameCount.first())) {
                    d << "Custom(" << Debug::nospace
                        << Trade::sceneFieldCustom(nameCount.first())
                        << Debug::nospace << ":" << Debug::nospace
                        << Debug::color(Debug::Color::Yellow)
                        << sceneFieldNames[sceneFieldCustom(nameCount.first())]
                        << Debug::nospace
                        << Debug::color(Debug::Color::Cyan) << ")";
                } else d << Debug::packed << nameCount.first();
                if(nameCount.second() != 1)
                    d << Debug::nospace << Utility::format("[{}]", nameCount.second());
                d << Debug::resetColor;
            }
        }
    }

    std::size_t totalAnimationDataSize = 0;
    for(const AnimationInfo& info: animationInfos) {
        Debug d{useColor};
        d << Debug::boldColor(Debug::Color::Default) << "Animation" << info.animation << Debug::nospace << ":" << Debug::resetColor;
        if(info.name) d << Debug::boldColor(Debug::Color::Yellow) << info.name << Debug::resetColor;

        d << Debug::newline << "  Duration: {" << Debug::nospace
            /** @todo have a nice packed printing for Range instead */
            << info.data.duration().min() << Debug::nospace << ","
            << info.data.duration().max() << Debug::nospace << "} ("
            << Debug::nospace << Utility::format("{:.1f}", info.data.data().size()/1024.0f) << "kB";
        if(info.data.dataFlags() != (Trade::DataFlag::Owned|Trade::DataFlag::Mutable))
            d << Debug::nospace << "," << Debug::packed
                << Debug::color(Debug::Color::Green)
                << info.data.dataFlags() << Debug::resetColor;
        d << Debug::nospace << ")";

        for(UnsignedInt i = 0; i != info.data.trackCount(); ++i) {
            d << Debug::newline << "  Track" << i << Debug::nospace << ":"
                << Debug::packed << Debug::boldColor(Debug::Color::Default)
                << info.data.trackTargetType(i)
                << Debug::color(Debug::Color::Blue) << "@"
                << Debug::packed << Debug::color(Debug::Color::Cyan)
                << info.data.trackType(i) << Debug::resetColor;
            if(info.data.trackType(i) != info.data.trackResultType(i))
                d << Debug::color(Debug::Color::Blue) << "->"
                    << Debug::packed << Debug::color(Debug::Color::Cyan)
                    << info.data.trackResultType(i) << Debug::resetColor;
            d << Debug::nospace << "," << info.data.track(i).size()
                << "keyframes";
            if(info.data.track(i).duration() != info.data.duration())
                d << Debug::newline << "    Duration: {" << Debug::nospace
                    /** @todo have a nice packed printing for Range instead */
                    << info.data.track(i).duration().min() << Debug::nospace
                    << "," << info.data.track(i).duration().max()
                    << Debug::nospace << "}";
            d << Debug::newline
                << "    Interpolation:"
                << Debug::packed << Debug::color(info.data.track(i).interpolation() == Animation::Interpolation::Custom ? Debug::Color::Yellow : Debug::Color::Cyan)
                << info.data.track(i).interpolation() << Debug::resetColor
                << Debug::nospace << "," << Debug::packed
                << Debug::color(Debug::Color::Cyan)
                << info.data.track(i).before() << Debug::resetColor
                << Debug::nospace << "," << Debug::packed
                << Debug::color(Debug::Color::Cyan)
                << info.data.track(i).after() << Debug::resetColor;
            /** @todo might be useful to show bounds here as well, though not
                so much for things like complex numbers or quats */
        }

        totalAnimationDataSize += info.data.data().size();
    }
    if(!animationInfos.isEmpty())
        Debug{} << "Total animation data size:" << Utility::format("{:.1f}", totalAnimationDataSize/1024.0f) << "kB";

    for(const SkinInfo& info: skinInfos) {
        Debug d{useColor};
        d << Debug::boldColor(Debug::Color::Default) << (info.twoDimensions ? "2D skin" : "3D skin") << info.skin
            << Debug::resetColor;

        /* Print reference count only if there actually are scenes and they
           were parsed, otherwise this information is useless */
        if((info.twoDimensions && skin2DReferenceCount) ||
           (!info.twoDimensions && skin3DReferenceCount))
        {
            const UnsignedInt count = info.twoDimensions ? skin2DReferenceCount[info.skin] : skin3DReferenceCount[info.skin];
            if(!count) d << Debug::color(Debug::Color::Red);
            d << "(referenced by" << count << "objects)";
            if(!count) d << Debug::resetColor;
        }

        d << Debug::boldColor(Debug::Color::Default) << Debug::nospace << ":"
            << Debug::resetColor;
        if(info.name) d << Debug::boldColor(Debug::Color::Yellow)
            << info.name << Debug::resetColor;

        d << Debug::newline << " " << info.jointCount << "joints";
    }

    for(const LightInfo& info: lightInfos) {
        Debug d{useColor};
        d << Debug::boldColor(Debug::Color::Default) << "Light" << info.light << Debug::resetColor;

        /* Print reference count only if there actually are scenes and they
           were parsed, otherwise this information is useless */
        if(lightReferenceCount) {
            const UnsignedInt count = lightReferenceCount[info.light];
            if(!count) d << Debug::color(Debug::Color::Red);
            d << "(referenced by" << count << "objects)";
            if(!count) d << Debug::resetColor;
        }

        d << Debug::boldColor(Debug::Color::Default) << Debug::nospace << ":"
            << Debug::resetColor;
        if(info.name) d << Debug::boldColor(Debug::Color::Yellow)
            << info.name << Debug::resetColor;

        d << Debug::newline << "  Type:" << Debug::packed
            << Debug::color(Debug::Color::Cyan)
            << info.data.type() << Debug::resetColor;
        if(info.data.type() == Trade::LightData::Type::Spot)
            d << Debug::nospace << "," << Debug::packed
                << Deg(info.data.innerConeAngle()) << Debug::nospace
                << "° -" << Debug::packed << Deg(info.data.outerConeAngle())
                << Debug::nospace << "°";
        d << Debug::newline << "  Color:";
        if(useColor24) d << Debug::color
            << Math::pack<Color3ub>(info.data.color());
        d << Debug::packed << info.data.color();
        if(!Math::equal(info.data.intensity(), 1.0f))
            d << "*" << info.data.intensity();
        if(info.data.type() != Trade::LightData::Type::Ambient &&
           info.data.type() != Trade::LightData::Type::Directional)
            d << Debug::newline << "  Attenuation:" << Debug::packed
                << info.data.attenuation();
        if(info.data.range() != Constants::inf())
            d << Debug::newline << "  Range:" << Debug::packed
                << info.data.range();
    }

    for(const CameraInfo& info: cameraInfos) {
        Debug d{useColor};
        d << Debug::boldColor(Debug::Color::Default) << "Camera" << info.camera << Debug::resetColor;

        /* Print reference count only if there actually are scenes and they
           were parsed, otherwise this information is useless */
        if(cameraReferenceCount) {
            const UnsignedInt count = cameraReferenceCount[info.camera];
            if(!count) d << Debug::color(Debug::Color::Red);
            d << "(referenced by" << count << "objects)";
            if(!count) d << Debug::resetColor;
        }

        d << Debug::boldColor(Debug::Color::Default) << Debug::nospace << ":"
            << Debug::resetColor;
        if(info.name) d << Debug::boldColor(Debug::Color::Yellow)
            << info.name << Debug::resetColor;

        d << Debug::newline << "  Type:" << Debug::packed
            << Debug::color(Debug::Color::Cyan)
            << info.data.type() << Debug::resetColor << Debug::newline;
        /* Print orthographic cameras with size, perspective with FoV */
        if(info.data.type() == Trade::CameraType::Orthographic2D ||
           info.data.type() == Trade::CameraType::Orthographic3D) {
            d << "  Size:" << Debug::packed << info.data.size();
        } else if(info.data.type() == Trade::CameraType::Perspective3D) {
            d << "  FoV:" << Debug::packed << Deg(info.data.fov())
                << Debug::nospace << "°";
        }
        /* Near/far is implicitly 0 for 2D */
        if(info.data.type() != Trade::CameraType::Orthographic2D)
            d << Debug::nospace << "," << info.data.near() << "-" << info.data.far();
        d << Debug::newline << "  Aspect ratio:" << info.data.aspectRatio();
    }

    for(const MaterialInfo& info: materialInfos) {
        Debug d{useColor};
        d << Debug::boldColor(Debug::Color::Default) << "Material" << info.material << Debug::resetColor;

        /* Print reference count only if there actually are scenes and they
           were parsed, otherwise this information is useless */
        if(materialReferenceCount) {
            const UnsignedInt count = materialReferenceCount[info.material];
            if(!count) d << Debug::color(Debug::Color::Red);
            d << "(referenced by" << count << "objects)";
            if(!count) d << Debug::resetColor;
        }

        d << Debug::boldColor(Debug::Color::Default) << Debug::nospace << ":"
            << Debug::resetColor;
        if(info.name) d << Debug::boldColor(Debug::Color::Yellow) << info.name << Debug::resetColor;

        d << Debug::newline << "  Type:" << Debug::packed << Debug::color(Debug::Color::Cyan) << info.data.types() << Debug::resetColor;

        for(UnsignedInt i = 0; i != info.data.layerCount(); ++i) {
            /* Print extra layers with extra indent */
            const char* indent;
            if(info.data.layerCount() != 1 && i != 0) {
                d << Debug::newline << "  Layer" << i << Debug::nospace << ":";
                if(!info.data.layerName(i).isEmpty()) {
                    if(std::isupper(info.data.layerName(i)[0]))
                        d << Debug::boldColor(Debug::Color::Default);
                    else
                        d << Debug::color(Debug::Color::Yellow);
                    d << info.data.layerName(i) << Debug::resetColor;
                }
                indent = "   ";
            } else {
                d << Debug::newline << "  Base layer:";
                indent = "   ";
            }

            for(UnsignedInt j = 0; j != info.data.attributeCount(i); ++j) {
                /* Ignore layer name (which is always first) unless it's in the
                   base material, in which case we print it as it wouldn't
                   otherwise be shown anywhere */
                if(i && !j && info.data.attributeName(i, j) == " LayerName")
                    continue;

                d << Debug::newline << indent;
                if(std::isupper(info.data.attributeName(i, j)[0]))
                    d << Debug::boldColor(Debug::Color::Default);
                else
                    d << Debug::color(Debug::Color::Yellow);
                d << info.data.attributeName(i, j) << Debug::color(Debug::Color::Blue) << "@" << Debug::packed << Debug::color(Debug::Color::Cyan)
                    << info.data.attributeType(i, j) << Debug::resetColor << Debug::nospace
                    << ":";
                switch(info.data.attributeType(i, j)) {
                    #define _c(type) case Trade::MaterialAttributeType::type: \
                        d << Debug::packed << info.data.attribute<type>(i, j);           \
                        break;
                    /* LCOV_EXCL_START */
                    _c(Float)
                    _c(Deg)
                    _c(Rad)
                    _c(UnsignedInt)
                    _c(Int)
                    _c(UnsignedLong)
                    _c(Long)
                    _c(Vector2)
                    _c(Vector2ui)
                    _c(Vector2i)
                    /* Vector3 handled below */
                    _c(Vector3ui)
                    _c(Vector3i)
                    /* Vector4 handled below */
                    _c(Vector4ui)
                    _c(Vector4i)
                    _c(Matrix2x2)
                    _c(Matrix2x3)
                    _c(Matrix2x4)
                    _c(Matrix3x2)
                    _c(Matrix3x3)
                    _c(Matrix3x4)
                    _c(Matrix4x2)
                    _c(Matrix4x3)
                    /* LCOV_EXCL_STOP */
                    #undef _c
                    case Trade::MaterialAttributeType::Bool:
                        d << info.data.attribute<bool>(i, j);
                        break;
                    case Trade::MaterialAttributeType::Vector3:
                        /** @todo hasSuffix() might be more robust against
                            false positives, but KHR_materials_specular in glTF
                            uses ColorFactor :/ */
                        if(useColor24 && info.data.attributeName(i, j).contains("Color"_s))
                            d << Debug::color << Math::pack<Color3ub>(info.data.attribute<Vector3>(i, j));
                        d << Debug::packed << info.data.attribute<Vector3>(i, j);
                        break;
                    case Trade::MaterialAttributeType::Vector4:
                        /** @todo hasSuffix() might be more robust against
                            false positives, but KHR_materials_specular in glTF
                            uses ColorFactor :/ */
                        if(useColor24 && info.data.attributeName(i, j).contains("Color"_s))
                            d << Debug::color << Math::pack<Color3ub>(info.data.attribute<Vector4>(i, j).rgb());
                        d << Debug::packed << info.data.attribute<Vector4>(i, j);
                        break;
                    case Trade::MaterialAttributeType::Pointer:
                        d << info.data.attribute<const void*>(i, j);
                        break;
                    case Trade::MaterialAttributeType::MutablePointer:
                        d << info.data.attribute<void*>(i, j);
                        break;
                    case Trade::MaterialAttributeType::String:
                        d << info.data.attribute<Containers::StringView>(i, j);
                        break;
                    case Trade::MaterialAttributeType::TextureSwizzle:
                        d << Debug::packed << info.data.attribute<Trade::MaterialTextureSwizzle>(i, j);
                        break;
                }
            }
        }
    }

    std::size_t totalMeshDataSize = 0;
    for(const MeshInfo& info: meshInfos) {
        Debug d{useColor};
        if(info.level == 0) {
            d << Debug::boldColor(Debug::Color::Default) << "Mesh" << info.mesh << Debug::resetColor;

            /* Print reference count only if there actually are scenes and they
               were parsed, otherwise this information is useless */
            if(meshReferenceCount) {
                const UnsignedInt count = meshReferenceCount[info.mesh];
                if(!count) d << Debug::color(Debug::Color::Red);
                d << "(referenced by" << count << "objects)";
                if(!count) d << Debug::resetColor;
            }

            d << Debug::boldColor(Debug::Color::Default) << Debug::nospace << ":"
                << Debug::resetColor;
            if(info.name) d << Debug::boldColor(Debug::Color::Yellow) << info.name << Debug::resetColor;
            d << Debug::newline;
        }
        d << "  Level" << info.level << Debug::nospace << ":"
            << info.vertexCount << "vertices" << Debug::color(Debug::Color::Blue) << "@" << Debug::packed << Debug::color(Debug::Color::Cyan) << info.primitive << Debug::resetColor << "(" << Debug::nospace
            << Utility::format("{:.1f}", info.vertexDataSize/1024.0f)
            << "kB";
        if(info.vertexDataFlags != (Trade::DataFlag::Owned|Trade::DataFlag::Mutable))
            d << Debug::nospace << "," << Debug::packed
                << Debug::color(Debug::Color::Green)
                << info.vertexDataFlags << Debug::resetColor;
        d << Debug::nospace << ")";

        for(const MeshAttributeInfo& attribute: info.attributes) {
            d << Debug::newline << "   "
                << Debug::boldColor(Debug::Color::Default);
            if(Trade::isMeshAttributeCustom(attribute.name)) {
                d << "Custom(" << Debug::nospace
                    << Trade::meshAttributeCustom(attribute.name)
                    << Debug::nospace << ":" << Debug::nospace
                    << Debug::color(Debug::Color::Yellow)
                    << attribute.customName << Debug::nospace
                    << Debug::boldColor(Debug::Color::Default) << ")";
            } else d << Debug::packed << attribute.name;

            d << Debug::color(Debug::Color::Blue) << "@" << Debug::packed << Debug::color(Debug::Color::Cyan) << attribute.format;
            if(attribute.arraySize)
                d << Debug::nospace << Utility::format("[{}]", attribute.arraySize);
            d << Debug::resetColor;
            d << Debug::nospace << ", offset" << attribute.offset;
            d << Debug::nospace << ", stride"
                << attribute.stride;
            if(attribute.bounds)
                d << Debug::newline << "      Bounds:" << attribute.bounds;
        }

        if(info.indexType != MeshIndexType{}) {
            d << Debug::newline << "   " << info.indexCount << "indices" << Debug::color(Debug::Color::Blue) << "@"
                << Debug::packed << Debug::color(Debug::Color::Cyan) << info.indexType << Debug::resetColor << Debug::nospace << ", offset" << info.indexOffset << Debug::nospace << ", stride" << info.indexStride << "(" << Debug::nospace
                << Utility::format("{:.1f}", info.indexDataSize/1024.0f)
                << "kB";
            if(info.indexDataFlags != (Trade::DataFlag::Owned|Trade::DataFlag::Mutable))
                d << Debug::nospace << "," << Debug::packed
                    << Debug::color(Debug::Color::Green) << info.indexDataFlags << Debug::resetColor;
            d << Debug::nospace << ")";
            if(info.indexBounds)
                d << Debug::newline << "      Bounds:" << info.indexBounds;
        }

        totalMeshDataSize += info.vertexDataSize + info.indexDataSize;
    }
    if(!meshInfos.isEmpty())
        Debug{} << "Total mesh data size:" << Utility::format("{:.1f}", totalMeshDataSize/1024.0f) << "kB";

    for(const TextureInfo& info: textureInfos) {
        Debug d{useColor};
        d << Debug::boldColor(Debug::Color::Default) << "Texture" << info.texture << Debug::resetColor;

        /* Print reference count only if there actually are materials and they
           were parsed, otherwise this information is useless */
        if(textureReferenceCount) {
            const UnsignedInt count = textureReferenceCount[info.texture];
            if(!count) d << Debug::color(Debug::Color::Red);
            d << "(referenced by" << count << "material attributes)";
            if(!count) d << Debug::resetColor;
        }

        d << Debug::boldColor(Debug::Color::Default) << Debug::nospace << ":"
            << Debug::resetColor;
        if(info.name) d << Debug::boldColor(Debug::Color::Yellow)
            << info.name << Debug::resetColor;
        d << Debug::newline;
        d << "  Type:"
            << Debug::packed
            << Debug::color(Debug::Color::Cyan) << info.data.type()
            << Debug::resetColor << Debug::nospace << ", image"
            << info.data.image();
        d << Debug::newline << "  Minification, mipmap and magnification:"
            << Debug::packed << Debug::color(Debug::Color::Cyan)
            << info.data.minificationFilter() << Debug::nospace << ","
            << Debug::packed << Debug::color(Debug::Color::Cyan)
            << info.data.mipmapFilter() << Debug::nospace << ","
            << Debug::packed << Debug::color(Debug::Color::Cyan)
            << info.data.magnificationFilter() << Debug::resetColor;
        /** @todo show only the dimensions that matter for a particular texture
            type */
        d << Debug::newline << "  Wrapping:" << Debug::resetColor << "{" << Debug::nospace
            << Debug::packed << Debug::color(Debug::Color::Cyan)
            << info.data.wrapping()[0] << Debug::resetColor
            << Debug::nospace << "," << Debug::packed
            << Debug::color(Debug::Color::Cyan) << info.data.wrapping()[1]
            << Debug::resetColor << Debug::nospace << "," << Debug::packed
            << Debug::color(Debug::Color::Cyan) << info.data.wrapping()[1]
            << Debug::resetColor << Debug::nospace << "}";
    }

    Trade::Implementation::printImageInfo(useColor, imageInfos, image1DReferenceCount, image2DReferenceCount, image3DReferenceCount);

    return error;
}

}

}}}

#endif
