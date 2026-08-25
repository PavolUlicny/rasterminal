#include "tests/inline_bmp.h"
#include "tests/loader_util.h"

#include "src/loaders/image_sniff.h"
#include "src/loaders/mesh_assimp.h"

#include <assimp/AssertHandler.h>
#include <assimp/material.h>

#include <algorithm>
#include <cstring>
#include <string>
#include <string_view>
#include <vector>

namespace
{
    constexpr const char *kTriangleCollada = R"(<?xml version="1.0" encoding="utf-8"?>
<COLLADA xmlns="http://www.collada.org/2005/11/COLLADASchema" version="1.4.1">
  <asset><unit name="meter" meter="1"/><up_axis>Y_UP</up_axis></asset>
  <library_geometries>
    <geometry id="triangle" name="triangle"><mesh>
      <source id="positions">
        <float_array id="positions-array" count="9">0 0 0 1 0 0 0 1 0</float_array>
        <technique_common><accessor source="#positions-array" count="3" stride="3">
          <param name="X" type="float"/><param name="Y" type="float"/><param name="Z" type="float"/>
        </accessor></technique_common>
      </source>
      <vertices id="vertices"><input semantic="POSITION" source="#positions"/></vertices>
      <triangles count="1"><input semantic="VERTEX" source="#vertices" offset="0"/><p>0 1 2</p></triangles>
    </mesh></geometry>
  </library_geometries>
  <library_visual_scenes><visual_scene id="scene"><node id="node">
    <translate>2 3 4</translate><scale>-2 3 1</scale><instance_geometry url="#triangle"/>
  </node></visual_scene></library_visual_scenes>
  <scene><instance_visual_scene url="#scene"/></scene>
</COLLADA>)";

    constexpr const char *kUv1FallbackCollada = R"(<?xml version="1.0" encoding="utf-8"?>
<COLLADA xmlns="http://www.collada.org/2005/11/COLLADASchema" version="1.4.1">
  <library_images><image id="image"><init_from>rast_assimp_uv0.bmp</init_from></image></library_images>
  <library_effects><effect id="effect"><profile_COMMON>
    <newparam sid="surface"><surface type="2D"><init_from>image</init_from></surface></newparam>
    <newparam sid="sampler"><sampler2D><source>surface</source></sampler2D></newparam>
    <technique sid="common"><lambert><diffuse><texture texture="sampler" texcoord="CHANNEL1"/></diffuse></lambert></technique>
  </profile_COMMON></effect></library_effects>
  <library_materials><material id="material"><instance_effect url="#effect"/></material></library_materials>
  <library_geometries><geometry id="triangle"><mesh>
    <source id="positions"><float_array id="positions-array" count="9">0 0 0 1 0 0 0 1 0</float_array>
      <technique_common><accessor source="#positions-array" count="3" stride="3"><param name="X" type="float"/><param name="Y" type="float"/><param name="Z" type="float"/></accessor></technique_common>
    </source>
    <source id="uv0"><float_array id="uv0-array" count="6">0 0 1 0 0 1</float_array>
      <technique_common><accessor source="#uv0-array" count="3" stride="2"><param name="S" type="float"/><param name="T" type="float"/></accessor></technique_common>
    </source>
    <vertices id="vertices"><input semantic="POSITION" source="#positions"/></vertices>
    <triangles material="material-symbol" count="1"><input semantic="VERTEX" source="#vertices" offset="0"/><input semantic="TEXCOORD" source="#uv0" offset="1" set="0"/><p>0 0 1 1 2 2</p></triangles>
  </mesh></geometry></library_geometries>
  <library_visual_scenes><visual_scene id="scene"><node><instance_geometry url="#triangle"><bind_material><technique_common>
    <instance_material symbol="material-symbol" target="#material"><bind_vertex_input semantic="CHANNEL1" input_semantic="TEXCOORD" input_set="1"/></instance_material>
  </technique_common></bind_material></instance_geometry></node></visual_scene></library_visual_scenes>
  <scene><instance_visual_scene url="#scene"/></scene>
</COLLADA>)";

    constexpr unsigned char k1x1_transparent_red_tga[] = {
        0, 0, 2, 0, 0, 0, 0, 0, 0, 0, 0, 0, 1, 0, 1, 0, 32, 0x28, 0, 0, 255, 0,
    };

    constexpr const char *kMd5MeshTriangle = "MD5Version 10\n"
                                             "commandline \"\"\n\n"
                                             "numJoints 1\n"
                                             "numMeshes 1\n\n"
                                             "joints {\n"
                                             "\t\"origin\"\t-1 ( 0 0 0 ) ( 0 0 0 )\n"
                                             "}\n\n"
                                             "mesh {\n"
                                             "\tshader \"rast_assimp_absent_texture\"\n\n"
                                             "\tnumverts 3\n"
                                             "\tvert 0 ( 0.0 0.0 ) 0 1 0\n"
                                             "\tvert 1 ( 1.0 0.0 ) 1 1 0\n"
                                             "\tvert 2 ( 0.0 1.0 ) 2 1 0\n\n"
                                             "\tnumtris 1\n"
                                             "\ttri 0 0 1 2\n\n"
                                             "\tnumweights 3\n"
                                             "\tweight 0 0 1 ( 0 0 0 )\n"
                                             "\tweight 1 0 1 ( 1 0 0 )\n"
                                             "\tweight 2 0 1 ( 0 1 0 )\n"
                                             "}\n";

    std::string textured_x(const char *texture_name, float alpha)
    {
        return R"(xof 0303txt 0032
Frame Root {
  Mesh Triangle {
    3;
    0;0;0;,
    1;0;0;,
    0;1;0;;
    1;
    3;0,1,2;;
    MeshTextureCoords {
      3;
      0;0;,
      1;0;,
      0;1;;
    }
    MeshMaterialList {
      1;
      1;
      0;
      Material {
        0.2;0.4;0.6;)" +
               std::to_string(alpha) + R"(;;
        48.0;
        0.1;0.2;0.3;;
        0.05;0.1;0.15;;
        TextureFilename { ")" +
               texture_name + R"("; }
      }
    }
  }
})";
    }

    // Same triangle with a second texture whose name routes it to the HEIGHT slot.
    std::string textured_x_with_height(const char *texture_name, const char *height_name)
    {
        return R"(xof 0303txt 0032
Frame Root {
  Mesh Triangle {
    3;
    0;0;0;,
    1;0;0;,
    0;1;0;;
    1;
    3;0,1,2;;
    MeshTextureCoords {
      3;
      0;0;,
      1;0;,
      0;1;;
    }
    MeshMaterialList {
      1;
      1;
      0;
      Material {
        0.2;0.4;0.6;1.0;;
        48.0;
        0.1;0.2;0.3;;
        0.05;0.1;0.15;;
        TextureFilename { ")" +
               std::string(texture_name) + R"("; }
        TextureFilename { ")" +
               std::string(height_name) + R"("; }
      }
    }
  }
})";
    }

    constexpr const char *kTriangleFbx = R"(; FBX 7.3.0 project file
FBXHeaderExtension:  {
  FBXHeaderVersion: 1003
  FBXVersion: 7300
  Creator: "rasterminal test"
}
GlobalSettings:  {
  Version: 1000
  Properties70:  {
    P: "UpAxis", "int", "Integer", "",1
    P: "UpAxisSign", "int", "Integer", "",1
    P: "FrontAxis", "int", "Integer", "",2
    P: "FrontAxisSign", "int", "Integer", "",-1
    P: "CoordAxis", "int", "Integer", "",0
    P: "CoordAxisSign", "int", "Integer", "",1
    P: "UnitScaleFactor", "double", "Number", "",100
  }
}
Definitions:  {
  Version: 100
  Count: 2
  ObjectType: "Geometry" { Count: 1 }
  ObjectType: "Model" { Count: 1 }
}
Objects:  {
  Geometry: 1, "Geometry::Triangle", "Mesh" {
    Vertices: *9 { a: 0,0,0,1,0,0,0,1,0 }
    PolygonVertexIndex: *3 { a: 0,1,-3 }
  }
  Model: 2, "Model::Triangle", "Mesh" {
    Version: 232
    Properties70:  {
      P: "Lcl Translation", "Lcl Translation", "", "A",0,0,0
      P: "Lcl Rotation", "Lcl Rotation", "", "A",0,0,0
      P: "Lcl Scaling", "Lcl Scaling", "", "A",1,1,1
    }
  }
}
Connections:  {
  C: "OO",1,2
  C: "OO",2,0
}
)";

    // Assimp packs these channels as raw "rgba8880", not aiTexel fields.
    constexpr const char *kAmfTexturedTriangle = R"(<?xml version="1.0" encoding="utf-8"?>
<amf unit="millimeter">
  <texture id="rt" width="1" height="1" depth="1" type="grayscale">/w==</texture>
  <texture id="gt" width="1" height="1" depth="1" type="grayscale">gA==</texture>
  <texture id="bt" width="1" height="1" depth="1" type="grayscale">AA==</texture>
  <object id="obj"><mesh>
    <vertices>
      <vertex><coordinates><x>0</x><y>0</y><z>0</z></coordinates></vertex>
      <vertex><coordinates><x>1</x><y>0</y><z>0</z></coordinates></vertex>
      <vertex><coordinates><x>0</x><y>1</y><z>0</z></coordinates></vertex>
    </vertices>
    <volume>
      <triangle><v1>0</v1><v2>1</v2><v3>2</v3>
        <texmap rtexid="rt" gtexid="gt" btexid="bt">
          <utex1>0</utex1><utex2>1</utex2><utex3>0</utex3>
          <vtex1>0</vtex1><vtex2>0</vtex2><vtex3>1</vtex3>
        </texmap>
      </triangle>
    </volume>
  </mesh></object>
</amf>)";

    // Assimp reports AMF's tiled=true as Clamp.
    constexpr const char *kAmfTiledTexture = R"(<?xml version="1.0" encoding="utf-8"?>
<amf unit="millimeter">
  <texture id="rt" width="1" height="1" depth="1" type="grayscale" tiled="true">AA==</texture>
  <texture id="gt" width="1" height="1" depth="1" type="grayscale" tiled="true">AA==</texture>
  <texture id="bt" width="1" height="1" depth="1" type="grayscale" tiled="true">AA==</texture>
  <object id="obj"><mesh>
    <vertices>
      <vertex><coordinates><x>0</x><y>0</y><z>0</z></coordinates></vertex>
      <vertex><coordinates><x>1</x><y>0</y><z>0</z></coordinates></vertex>
      <vertex><coordinates><x>0</x><y>1</y><z>0</z></coordinates></vertex>
    </vertices>
    <volume>
      <triangle><v1>0</v1><v2>1</v2><v3>2</v3>
        <texmap rtexid="rt" gtexid="gt" btexid="bt">
          <utex1>0</utex1><utex2>1</utex2><utex3>0</utex3>
          <vtex1>0</vtex1><vtex2>0</vtex2><vtex3>1</vtex3>
        </texmap>
      </triangle>
    </volume>
  </mesh></object>
</amf>)";

    constexpr const char *kAmfVertexColor = R"(<?xml version="1.0" encoding="utf-8"?>
<amf unit="millimeter">
  <object id="obj"><mesh>
    <vertices>
      <vertex><coordinates><x>0</x><y>0</y><z>0</z></coordinates></vertex>
      <vertex><coordinates><x>1</x><y>0</y><z>0</z></coordinates></vertex>
      <vertex><coordinates><x>0</x><y>1</y><z>0</z></coordinates></vertex>
    </vertices>
    <volume>
      <triangle><v1>0</v1><v2>1</v2><v3>2</v3>
        <color><r>1</r><g>0.5</g><b>0</b><a>0.25</a></color>
      </triangle>
    </volume>
  </mesh></object>
</amf>)";

    constexpr const char *kZupSmd = R"(version 1
nodes
0 "root" -1
end
skeleton
time 0
0 0 0 0 0 0 0
end
triangles
none
0 0 0 0  0 0 1  0 0
0 1 0 0  0 0 1  1 0
0 0 0 2  0 0 1  0 1
end
)";

    constexpr const char *kBvhMotionOnly = R"(HIERARCHY
ROOT hips
{
  OFFSET 0 0 0
  CHANNELS 6 Xposition Yposition Zposition Zrotation Xrotation Yrotation
  End Site
  {
    OFFSET 0 10 0
  }
}
MOTION
Frames: 1
Frame Time: 0
0 0 0 0 0 0
)";

    // Assimp stores X3D repeatS=true in an enum where 1 means Clamp.
    constexpr const char *kX3dTexturedBox = R"(<?xml version="1.0" encoding="UTF-8"?>
<X3D profile="Interchange">
<Scene>
<Shape>
<Appearance>
<ImageTexture url="rast_assimp_red.bmp" repeatS="true" repeatT="true"/>
<Material diffuseColor="1 0 0"/>
</Appearance>
<Box size="1 1 1"/>
</Shape>
</Scene>
</X3D>)";

    // Assimp puts Collada's <bump> texture in aiTextureType_NORMALS.
    constexpr const char *kBumpCollada = R"(<?xml version="1.0" encoding="utf-8"?>
<COLLADA xmlns="http://www.collada.org/2005/11/COLLADASchema" version="1.4.1">
  <library_images><image id="bumpimg"><init_from>rast_assimp_height.bmp</init_from></image></library_images>
  <library_effects><effect id="effect"><profile_COMMON>
    <newparam sid="bumpsurface"><surface type="2D"><init_from>bumpimg</init_from></surface></newparam>
    <newparam sid="bumpsampler"><sampler2D><source>bumpsurface</source></sampler2D></newparam>
    <technique sid="common"><lambert>
      <diffuse><color>1 1 1 1</color></diffuse>
      <bump><texture texture="bumpsampler" texcoord="CHANNEL0"/></bump>
    </lambert></technique>
  </profile_COMMON></effect></library_effects>
  <library_materials><material id="material"><instance_effect url="#effect"/></material></library_materials>
  <library_geometries><geometry id="triangle"><mesh>
    <source id="positions"><float_array id="positions-array" count="9">0 0 0 1 0 0 0 1 0</float_array>
      <technique_common><accessor source="#positions-array" count="3" stride="3"><param name="X" type="float"/><param name="Y" type="float"/><param name="Z" type="float"/></accessor></technique_common>
    </source>
    <source id="uv0"><float_array id="uv0-array" count="6">0 0 1 0 0 1</float_array>
      <technique_common><accessor source="#uv0-array" count="3" stride="2"><param name="S" type="float"/><param name="T" type="float"/></accessor></technique_common>
    </source>
    <vertices id="vertices"><input semantic="POSITION" source="#positions"/></vertices>
    <triangles material="material-symbol" count="1"><input semantic="VERTEX" source="#vertices" offset="0"/><input semantic="TEXCOORD" source="#uv0" offset="1" set="0"/><p>0 0 1 1 2 2</p></triangles>
  </mesh></geometry></library_geometries>
  <library_visual_scenes><visual_scene id="scene"><node><instance_geometry url="#triangle"><bind_material><technique_common>
    <instance_material symbol="material-symbol" target="#material"/>
  </technique_common></bind_material></instance_geometry></node></visual_scene></library_visual_scenes>
  <scene><instance_visual_scene url="#scene"/></scene>
</COLLADA>)";

    // Assimp gives this material a template shininess of 10.
    constexpr const char *kPhongNoShininessCollada = R"(<?xml version="1.0" encoding="utf-8"?>
<COLLADA xmlns="http://www.collada.org/2005/11/COLLADASchema" version="1.4.1">
  <library_effects><effect id="effect"><profile_COMMON>
    <technique sid="common"><phong>
      <diffuse><color>1 1 1 1</color></diffuse>
    </phong></technique>
  </profile_COMMON></effect></library_effects>
  <library_materials><material id="material"><instance_effect url="#effect"/></material></library_materials>
  <library_geometries><geometry id="triangle"><mesh>
    <source id="positions"><float_array id="positions-array" count="9">0 0 0 1 0 0 0 1 0</float_array>
      <technique_common><accessor source="#positions-array" count="3" stride="3"><param name="X" type="float"/><param name="Y" type="float"/><param name="Z" type="float"/></accessor></technique_common>
    </source>
    <vertices id="vertices"><input semantic="POSITION" source="#positions"/></vertices>
    <triangles material="material-symbol" count="1"><input semantic="VERTEX" source="#vertices" offset="0"/><p>0 1 2</p></triangles>
  </mesh></geometry></library_geometries>
  <library_visual_scenes><visual_scene id="scene"><node><instance_geometry url="#triangle"><bind_material><technique_common>
    <instance_material symbol="material-symbol" target="#material"/>
  </technique_common></bind_material></instance_geometry></node></visual_scene></library_visual_scenes>
  <scene><instance_visual_scene url="#scene"/></scene>
</COLLADA>)";

    constexpr const char *kPhongCollada = R"(<?xml version="1.0" encoding="utf-8"?>
<COLLADA xmlns="http://www.collada.org/2005/11/COLLADASchema" version="1.4.1">
  <library_effects><effect id="effect"><profile_COMMON>
    <technique sid="common"><phong>
      <diffuse><color>1 1 1 1</color></diffuse>
      <shininess><float>80</float></shininess>
    </phong></technique>
  </profile_COMMON></effect></library_effects>
  <library_materials><material id="material"><instance_effect url="#effect"/></material></library_materials>
  <library_geometries><geometry id="triangle"><mesh>
    <source id="positions"><float_array id="positions-array" count="9">0 0 0 1 0 0 0 1 0</float_array>
      <technique_common><accessor source="#positions-array" count="3" stride="3"><param name="X" type="float"/><param name="Y" type="float"/><param name="Z" type="float"/></accessor></technique_common>
    </source>
    <vertices id="vertices"><input semantic="POSITION" source="#positions"/></vertices>
    <triangles material="material-symbol" count="1"><input semantic="VERTEX" source="#vertices" offset="0"/><p>0 1 2</p></triangles>
  </mesh></geometry></library_geometries>
  <library_visual_scenes><visual_scene id="scene"><node><instance_geometry url="#triangle"><bind_material><technique_common>
    <instance_material symbol="material-symbol" target="#material"/>
  </technique_common></bind_material></instance_geometry></node></visual_scene></library_visual_scenes>
  <scene><instance_visual_scene url="#scene"/></scene>
</COLLADA>)";

    constexpr const char *kDxfDefaultColorFace = R"(0
SECTION
2
ENTITIES
0
3DFACE
8
layer0
10
0.0
20
0.0
30
0.0
11
1.0
21
0.0
31
0.0
12
1.0
22
1.0
32
0.0
13
0.0
23
1.0
33
0.0
62
7
0
ENDSEC
0
EOF
)";

    // FBXConverter derives ROUGHNESS_FACTOR from the authored SHININESS.
    constexpr const char *kFbxPhongShininess = R"(; FBX 7.3.0 project file
FBXHeaderExtension:  {
  FBXHeaderVersion: 1003
  FBXVersion: 7300
}
GlobalSettings:  {
  Version: 1000
  Properties70:  {
    P: "UpAxis", "int", "Integer", "",1
    P: "UpAxisSign", "int", "Integer", "",1
  }
}
Definitions:  {
  Version: 100
  Count: 2
  ObjectType: "Geometry" { Count: 1 }
  ObjectType: "Model" { Count: 1 }
  ObjectType: "Material" { Count: 1 }
}
Objects:  {
  Geometry: 1, "Geometry::Triangle", "Mesh" {
    Vertices: *9 { a: 0,0,0,1,0,0,0,1,0 }
    PolygonVertexIndex: *3 { a: 0,1,-3 }
    LayerElementMaterial: 0 {
      Version: 101
      Name: ""
      MappingInformationType: "AllSame"
      ReferenceInformationType: "IndexToDirect"
      Materials: *1 { a: 0 }
    }
    Layer: 0 {
      Version: 100
      LayerElement:  {
        Type: "LayerElementMaterial"
        TypedIndex: 0
      }
    }
  }
  Model: 2, "Model::Triangle", "Mesh" {
    Version: 232
    Properties70:  {
      P: "Lcl Translation", "Lcl Translation", "", "A",0,0,0
      P: "Lcl Rotation", "Lcl Rotation", "", "A",0,0,0
      P: "Lcl Scaling", "Lcl Scaling", "", "A",1,1,1
    }
  }
  Material: 3, "Material::mat", "" {
    Version: 102
    ShadingModel: "phong"
    MultiLayer: 0
    Properties70:  {
      P: "DiffuseColor", "Color", "", "A",0.5,0.5,0.5
      P: "SpecularColor", "Color", "", "A",0.8,0.8,0.8
      P: "SpecularFactor", "Number", "", "A",0.5
      P: "ShininessExponent", "Number", "", "A",30
    }
  }
}
Connections:  {
  C: "OO",1,2
  C: "OO",2,0
  C: "OO",3,2
}
)";

    constexpr const char *kFbxAuthoredAmbient = R"(; FBX 7.3.0 project file
FBXHeaderExtension:  {
  FBXHeaderVersion: 1003
  FBXVersion: 7300
}
GlobalSettings:  {
  Version: 1000
  Properties70:  {
    P: "UpAxis", "int", "Integer", "",1
    P: "UpAxisSign", "int", "Integer", "",1
  }
}
Definitions:  {
  Version: 100
  Count: 2
  ObjectType: "Geometry" { Count: 1 }
  ObjectType: "Model" { Count: 1 }
  ObjectType: "Material" { Count: 1 }
}
Objects:  {
  Geometry: 1, "Geometry::Triangle", "Mesh" {
    Vertices: *9 { a: 0,0,0,1,0,0,0,1,0 }
    PolygonVertexIndex: *3 { a: 0,1,-3 }
    LayerElementMaterial: 0 {
      Version: 101
      Name: ""
      MappingInformationType: "AllSame"
      ReferenceInformationType: "IndexToDirect"
      Materials: *1 { a: 0 }
    }
    Layer: 0 {
      Version: 100
      LayerElement:  {
        Type: "LayerElementMaterial"
        TypedIndex: 0
      }
    }
  }
  Model: 2, "Model::Triangle", "Mesh" {
    Version: 232
    Properties70:  {
      P: "Lcl Translation", "Lcl Translation", "", "A",0,0,0
      P: "Lcl Rotation", "Lcl Rotation", "", "A",0,0,0
      P: "Lcl Scaling", "Lcl Scaling", "", "A",1,1,1
    }
  }
  Material: 3, "Material::mat", "" {
    Version: 102
    ShadingModel: "phong"
    MultiLayer: 0
    Properties70:  {
      P: "DiffuseColor", "Color", "", "A",0.5,0.5,0.5
      P: "AmbientColor", "Color", "", "A",0.1,0.2,0.3
    }
  }
}
Connections:  {
  C: "OO",1,2
  C: "OO",2,0
  C: "OO",3,2
}
)";

    void append_i16_le(std::string &out, int16_t v)
    {
        out.push_back(static_cast<char>(v & 0xFF));
        out.push_back(static_cast<char>((static_cast<uint16_t>(v) >> 8) & 0xFF));
    }

    // SCAL removes Assimp's default x30 terrain scale. Chunks are four-byte aligned.
    std::string terragen_flat_grid()
    {
        std::string out = "TERRAGENTERRAIN ";
        out += "SCAL";
        emit_f32_le(out, 1.0f);
        emit_f32_le(out, 1.0f);
        emit_f32_le(out, 1.0f);
        out += "SIZE";
        append_i16_le(out, 3); // grid becomes value + 1 on each axis
        out.append(2, '\0');   // alignment padding
        out += "ALTW";
        append_i16_le(out, 0); // hscale 0 is replaced by 1 inside the importer
        append_i16_le(out, 0); // base height
        for (int i = 0; i < 16; i++)
        {
            append_i16_le(out, 100);
        }
        return out;
    }

    std::string terragen_hostile_oversized_grid()
    {
        std::string out = "TERRAGENTERRAIN ";
        out += "SIZE";
        append_i16_le(out, static_cast<int16_t>(0xFFFF)); // grid 65536 x 65536
        out.append(2, '\0');
        out += "ALTW";
        append_i16_le(out, 0);
        append_i16_le(out, 0);
        return out;
    }

    // XPTS and YPTS exercise alignment across two-byte chunk bodies.
    std::string terragen_declared_grid()
    {
        std::string out = "TERRAGENTERRAIN ";
        out += "XPTS";
        append_i16_le(out, 129);
        out.append(2, '\0');
        out += "YPTS";
        append_i16_le(out, 129);
        out.append(2, '\0');
        out += "ALTW";
        append_i16_le(out, 0); // hscale 0 is replaced by 1 inside the importer
        append_i16_le(out, 0); // base height
        for (int i = 0; i < 129 * 129; i++)
        {
            append_i16_le(out, 0);
        }
        return out;
    }

    void bounds(const Mesh &mesh, vec3 &lo, vec3 &hi)
    {
        lo = hi = mesh.vertices.front().pos;
        for (const Vertex &vertex : mesh.vertices)
        {
            lo.x = std::min(lo.x, vertex.pos.x);
            lo.y = std::min(lo.y, vertex.pos.y);
            lo.z = std::min(lo.z, vertex.pos.z);
            hi.x = std::max(hi.x, vertex.pos.x);
            hi.y = std::max(hi.y, vertex.pos.y);
            hi.z = std::max(hi.z, vertex.pos.z);
        }
    }

    // Captures stderr across assertions. Check ok before trusting text().
    struct StderrCapture
    {
        int saved_err = -1;
        std::FILE *capture = nullptr;
        bool ok = false;

        StderrCapture()
            : saved_err(test_dup(TEST_STDERR)), capture(saved_err >= 0 ? std::tmpfile() : nullptr),
              ok(capture != nullptr && test_dup2(test_fileno(capture), TEST_STDERR) >= 0)
        {
        }
        ~StderrCapture()
        {
            if (saved_err >= 0)
            {
                test_dup2(saved_err, TEST_STDERR);
                test_close(saved_err);
            }
            if (capture != nullptr)
            {
                std::fclose(capture);
            }
        }
        [[nodiscard]] std::string text() const
        {
            std::string out;
            if (capture == nullptr)
            {
                return out;
            }
            // Rewind the shared file offset before reading.
            test_seek(test_fileno(capture), 0, SEEK_SET);
            char buffer[512] = {};
            size_t n = 0;
            while ((n = std::fread(buffer, 1, sizeof buffer, capture)) > 0 // NOLINT(clang-analyzer-unix.Stream)
            )
            {
                out.append(buffer, n);
            }
            return out;
        }
        StderrCapture(const StderrCapture &) = delete;
        StderrCapture &operator=(const StderrCapture &) = delete;
        StderrCapture(StderrCapture &&) = delete;
        StderrCapture &operator=(StderrCapture &&) = delete;
    };
} // namespace

TEST(assimp, embedded_format_hint_is_bounded_to_the_descriptor)
{
    aiTexture texture;
    std::memcpy(texture.achFormatHint, "rgba8888X", sizeof texture.achFormatHint);
    ASSERT_TRUE(assimp_detail::embedded_format_hint(texture) == "rgba8888");

    std::memset(texture.achFormatHint, 'X', sizeof texture.achFormatHint);
    std::memcpy(texture.achFormatHint, "webp", 5);
    ASSERT_TRUE(assimp_detail::embedded_format_hint(texture) == "webp");
}

// HL1 MDL and AMF use the same hint for opposite byte layouts.
TEST(assimp, embedded_texel_layout_gates_raw_bytes_on_amf_origin)
{
    ASSERT_EQ(embedded_texel_layout("rgba8888", true), TexelLayout::RgbaInterleaved);
    ASSERT_EQ(embedded_texel_layout("rgba8880", true), TexelLayout::RgbaInterleaved);
    ASSERT_EQ(embedded_texel_layout("rgba8000", true), TexelLayout::RgbaInterleaved);
    ASSERT_EQ(embedded_texel_layout("rgba8888", false), TexelLayout::ArgbTexels);
    ASSERT_EQ(embedded_texel_layout("", true), TexelLayout::ArgbTexels);
    ASSERT_EQ(embedded_texel_layout("", false), TexelLayout::ArgbTexels);
    ASSERT_EQ(embedded_texel_layout("png", true), TexelLayout::ArgbTexels);
    // Ignore AMF's uninitialized ninth hint byte.
    ASSERT_EQ(embedded_texel_layout(std::string_view("rgba8880B", 9), true), TexelLayout::RgbaInterleaved);
    ASSERT_EQ(embedded_texel_layout("rgba8828", true), TexelLayout::ArgbTexels);
}

TEST(assimp, off_quad_uses_assimp_and_triangulates)
{
    TmpFile file(tmp_path("rast_assimp_quad.OFF"), "OFF\n4 1 0\n0 0 0\n1 0 0\n1 1 0\n0 1 0\n4 0 1 2 3\n");
    const Mesh mesh = load_ok(file.path);
    ASSERT_EQ(mesh.triangles.size(), size_t{ 2 });
    ASSERT_EQ(mesh.tangents.size(), mesh.vertices.size());
}

// Assimp leaves COFF's 0..255 colors unnormalized.
TEST(assimp, off_integer_vertex_colors_normalize)
{
    TmpFile file(
        tmp_path("rast_assimp_colors.off"),
        "COFF\n3 1 0\n0 0 0 255 0 0 255\n1 0 0 255 0 0 255\n0 1 0 255 0 0 255\n3 0 1 2 0 0 0 255\n"
    );
    const Mesh mesh = load_ok(file.path);
    ASSERT_TRUE(mesh.has_vertex_colors);
    for (const vec3 &color : mesh.vertex_colors)
    {
        ASSERT_NEAR(color.x, 1.0f, 1e-4f);
        ASSERT_NEAR(color.y, 0.0f, 1e-4f);
        ASSERT_NEAR(color.z, 0.0f, 1e-4f);
    }
}

// Motion-only MD5 files must not become synthetic stick figures.
TEST(assimp, md5_motion_sidecar_is_rejected)
{
    TmpFile file(tmp_path("rast_assimp_anim.md5anim"), "MD5Version 10\nnumFrames 1\n");
    assert_rejects(file.path);
}

TEST(assimp, collada_flattens_node_transform)
{
    TmpFile file(tmp_path("rast_assimp_transform.dae"), kTriangleCollada);
    const Mesh mesh = load_ok(file.path);
    vec3 lo, hi;
    bounds(mesh, lo, hi);
    ASSERT_NEAR(lo.x, 0.0f, 1e-5f);
    ASSERT_NEAR(lo.y, 3.0f, 1e-5f);
    ASSERT_NEAR(lo.z, 4.0f, 1e-5f);
    ASSERT_NEAR(hi.x, 2.0f, 1e-5f);
    ASSERT_NEAR(hi.y, 6.0f, 1e-5f);
    ASSERT_NEAR(hi.z, 4.0f, 1e-5f);

    const Triangle &triangle = mesh.triangles.front();
    const vec3 a = mesh.vertices[triangle.v[0]].pos;
    const vec3 b = mesh.vertices[triangle.v[1]].pos;
    const vec3 c = mesh.vertices[triangle.v[2]].pos;
    const vec3 geometric = cross(b - a, c - a);
    ASSERT_TRUE(dot(geometric, mesh.vertices[triangle.v[0]].normal) > 0.0f);
}

TEST(assimp, missing_second_uv_set_falls_back_to_first)
{
    TmpFile texture(tmp_path("rast_assimp_uv0.bmp"), k1x1_red_bmp, sizeof(k1x1_red_bmp));
    TmpFile file(tmp_path("rast_assimp_uv1_fallback.dae"), kUv1FallbackCollada);
    const Mesh mesh = load_ok(file.path);
    const Material &material = mesh.mat_at(mesh.triangles.front().material_idx);
    ASSERT_EQ(material.diffuse_map.uv_set, uint8_t{ 1 });
    ASSERT_TRUE(mesh.has_uv1);
    ASSERT_EQ(mesh.uv1.size(), mesh.vertices.size());
    for (size_t i = 0; i < mesh.vertices.size(); i++)
    {
        ASSERT_NEAR(mesh.uv1[i].x, mesh.vertices[i].uv.x, 1e-6f);
        ASSERT_NEAR(mesh.uv1[i].y, mesh.vertices[i].uv.y, 1e-6f);
    }
}

TEST(assimp, x_maps_material_texture_and_alpha)
{
    TmpFile texture(tmp_path("rast_assimp_red.bmp"), k1x1_red_bmp, sizeof(k1x1_red_bmp));
    TmpFile file(tmp_path("rast_assimp_material.x"), textured_x("rast_assimp_red.bmp", 0.5f));
    const Mesh mesh = load_ok(file.path);
    ASSERT_TRUE(mesh.materials.size() >= 2);
    const Material &material = mesh.mat_at(mesh.triangles.front().material_idx);
    ASSERT_NEAR(material.diffuse.x, 0.2f, 1e-5f);
    ASSERT_NEAR(material.diffuse.y, 0.4f, 1e-5f);
    ASSERT_NEAR(material.diffuse.z, 0.6f, 1e-5f);
    ASSERT_NEAR(material.alpha, 0.5f, 1e-5f);
    ASSERT_TRUE(material.blend);
    ASSERT_EQ(material.diffuse_map.tex, 0);
    ASSERT_EQ(mesh.textures.size(), size_t{ 1 });
    ASSERT_EQ(mesh.textures[0].pixels[0], uint8_t{ 255 });
}

TEST(assimp, x_preserves_fully_transparent_materials)
{
    TmpFile texture(tmp_path("rast_assimp_zero_alpha.bmp"), k1x1_red_bmp, sizeof(k1x1_red_bmp));
    TmpFile file(tmp_path("rast_assimp_zero_alpha.x"), textured_x("rast_assimp_zero_alpha.bmp", 0.0f));
    const Mesh mesh = load_ok(file.path);
    const Material &material = mesh.mat_at(mesh.triangles.front().material_idx);
    ASSERT_NEAR(material.alpha, 0.0f, 1e-5f);
    ASSERT_TRUE(material.blend);
    ASSERT_TRUE(mesh.has_transparent);
}

TEST(assimp, diffuse_texture_alpha_does_not_enable_blending)
{
    TmpFile texture(
        tmp_path("rast_assimp_transparent.tga"), k1x1_transparent_red_tga, sizeof(k1x1_transparent_red_tga)
    );
    TmpFile file(tmp_path("rast_assimp_opaque_texture.x"), textured_x("rast_assimp_transparent.tga", 1.0f));
    const Mesh mesh = load_ok(file.path);
    const Material &material = mesh.mat_at(mesh.triangles.front().material_idx);
    ASSERT_FALSE(material.blend);
    ASSERT_FALSE(mesh.has_transparent);
    ASSERT_EQ(mesh.textures[static_cast<size_t>(material.diffuse_map.tex)].pixels[3], uint8_t{ 0 });
}

TEST(assimp, fbx_ascii_sanity)
{
    TmpFile file(tmp_path("rast_assimp_triangle.fbx"), kTriangleFbx);
    const Mesh mesh = load_ok(file.path);
    ASSERT_EQ(mesh.triangles.size(), size_t{ 1 });
}

TEST(assimp, amf_embedded_texture_reads_channels_by_format_hint)
{
    TmpFile file(tmp_path("rast_assimp_embedded.amf"), kAmfTexturedTriangle);
    const Mesh mesh = load_ok(file.path);
    ASSERT_EQ(mesh.textures.size(), size_t{ 1 });
    const Texture &texture = mesh.textures.front();
    ASSERT_EQ(texture.width, 1);
    ASSERT_EQ(texture.height, 1);
    ASSERT_EQ(texture.pixels[0], uint8_t{ 255 });
    ASSERT_EQ(texture.pixels[1], uint8_t{ 128 });
    ASSERT_EQ(texture.pixels[2], uint8_t{ 0 });
    ASSERT_EQ(texture.pixels[3], uint8_t{ 255 });
    // The importer writes Tiled?1:0 into a key whose value space says 1 = Clamp, so an
    // untiled (stretch-once) texture lands as Repeat before our swap corrects it to Clamp.
    ASSERT_EQ(texture.wrap_s, WrapMode::Clamp);
    ASSERT_EQ(texture.wrap_t, WrapMode::Clamp);
}

TEST(assimp, amf_tiled_texture_maps_to_repeat_wrap)
{
    TmpFile file(tmp_path("rast_assimp_tiled.amf"), kAmfTiledTexture);
    const Mesh mesh = load_ok(file.path);
    ASSERT_EQ(mesh.textures.size(), size_t{ 1 });
    const Texture &texture = mesh.textures.front();
    // Authored tiling means repeat; the importer's raw mapping-mode value reads as Clamp.
    ASSERT_EQ(texture.wrap_s, WrapMode::Repeat);
    ASSERT_EQ(texture.wrap_t, WrapMode::Repeat);
}

TEST(assimp, amf_translucent_vertex_color_enables_transparent_pass)
{
    TmpFile file(tmp_path("rast_assimp_vcolor.amf"), kAmfVertexColor);
    const Mesh mesh = load_ok(file.path);
    ASSERT_TRUE(mesh.has_vertex_colors);
    ASSERT_TRUE(mesh.has_vertex_alpha);
    ASSERT_TRUE(mesh.has_transparent);
    ASSERT_EQ(mesh.vertex_colors.size(), mesh.vertices.size());
    ASSERT_EQ(mesh.vertex_alpha.size(), mesh.vertices.size());
    for (size_t i = 0; i < mesh.vertices.size(); i++)
    {
        ASSERT_NEAR(mesh.vertex_colors[i].x, 1.0f, 1e-5f);
        ASSERT_NEAR(mesh.vertex_colors[i].y, 0.5f, 1e-5f);
        ASSERT_NEAR(mesh.vertex_colors[i].z, 0.0f, 1e-5f);
        ASSERT_NEAR(mesh.vertex_alpha[i], 0.25f, 1e-5f);
    }
}

// An AMF volume with no color source arrives as the importer's {0,0,0,0} default; reading
// that literally tinted plain models black and routed them into the blend pass.
constexpr const char *kAmfUncoloredTriangle = R"(<?xml version="1.0" encoding="utf-8"?>
<amf unit="millimeter">
  <object id="obj"><mesh>
    <vertices>
      <vertex><coordinates><x>0</x><y>0</y><z>0</z></coordinates></vertex>
      <vertex><coordinates><x>1</x><y>0</y><z>0</z></coordinates></vertex>
      <vertex><coordinates><x>0</x><y>1</y><z>0</z></coordinates></vertex>
    </vertices>
    <volume>
      <triangle><v1>0</v1><v2>1</v2><v3>2</v3></triangle>
    </volume>
  </mesh></object>
</amf>)";

TEST(assimp, amf_uncolored_volume_is_not_read_as_black_transparent)
{
    TmpFile file(tmp_path("rast_assimp_uncolored.amf"), kAmfUncoloredTriangle);
    const Mesh mesh = load_ok(file.path);
    ASSERT_FALSE(mesh.has_vertex_alpha);
    ASSERT_FALSE(mesh.has_transparent);
    if (mesh.has_vertex_colors)
    {
        for (const vec3 &color : mesh.vertex_colors)
        {
            ASSERT_NEAR(color.x, 1.0f, 1e-5f);
            ASSERT_NEAR(color.y, 1.0f, 1e-5f);
            ASSERT_NEAR(color.z, 1.0f, 1e-5f);
        }
    }
}

// AMF is Z-up per ISO/ASTM 52915 and its importer converts nothing.
TEST(assimp, amf_zup_geometry_is_remapped_to_yup)
{
    TmpFile file(tmp_path("rast_assimp_zup.amf"), R"(<?xml version="1.0" encoding="utf-8"?>
<amf unit="millimeter">
  <object id="obj"><mesh>
    <vertices>
      <vertex><coordinates><x>0</x><y>0</y><z>0</z></coordinates></vertex>
      <vertex><coordinates><x>1</x><y>0</y><z>0</z></coordinates></vertex>
      <vertex><coordinates><x>0</x><y>-1</y><z>2</z></coordinates></vertex>
    </vertices>
    <volume>
      <triangle><v1>0</v1><v2>1</v2><v3>2</v3></triangle>
    </volume>
  </mesh></object>
</amf>)");
    const Mesh mesh = load_ok(file.path);
    vec3 lo, hi;
    bounds(mesh, lo, hi);
    // Source elevation on +Z lands on +Y with the footprint spanning x/z.
    ASSERT_NEAR(hi.y, 2.0f, 1e-5f);
}

// Native loader failures must not reach Assimp.
TEST(assimp, malformed_native_model_is_not_retried_through_assimp)
{
    TmpFile file(tmp_path("rast_assimp_no_retry.obj"), "this is not an obj\n");
    StderrCapture captured;
    ASSERT_TRUE(captured.ok);
    assert_rejects(file.path);
    const std::string output = captured.text();
    ASSERT_TRUE(output.find("Assimp") == std::string::npos);
}

TEST(assimp, smd_zup_geometry_is_remapped_to_yup)
{
    TmpFile file(tmp_path("rast_assimp_zup.smd"), kZupSmd);
    const Mesh mesh = load_ok(file.path);
    vec3 lo, hi;
    bounds(mesh, lo, hi);
    // Source space puts the elevation on +Z; after the remap it lands on +Y with the
    // ground plane spanning x/z.
    ASSERT_NEAR(hi.y, 2.0f, 1e-5f);
    ASSERT_NEAR(lo.z, 0.0f, 1e-5f);
    ASSERT_NEAR(hi.z, 0.0f, 1e-5f);
}

TEST(assimp, terragen_grid_remaps_to_yup_and_faces_the_sky)
{
    TmpFile file(tmp_path("rast_assimp_terrain.ter"), terragen_flat_grid());
    const Mesh mesh = load_ok(file.path);
    vec3 lo, hi;
    bounds(mesh, lo, hi);
    ASSERT_NEAR(lo.y, 100.0f, 1e-3f);
    ASSERT_NEAR(hi.y, 100.0f, 1e-3f);
    ASSERT_NEAR(lo.z, -3.0f, 1e-5f);
    ASSERT_NEAR(hi.z, 0.0f, 1e-5f);
    // The importer winds its quads clockwise seen from above; the reversal must leave
    // every triangle facing +Y so backface culling keeps the surface visible.
    for (const Triangle &triangle : mesh.triangles)
    {
        const vec3 &a = mesh.vertices[triangle.v[0]].pos;
        const vec3 &b = mesh.vertices[triangle.v[1]].pos;
        const vec3 &c = mesh.vertices[triangle.v[2]].pos;
        ASSERT_TRUE(cross(b - a, c - a).y > 0.0f);
    }
}

TEST(assimp, terragen_oversized_declared_grid_fails_in_the_prescreen)
{
    TmpFile file(tmp_path("rast_assimp_hostile.ter"), terragen_hostile_oversized_grid());
    StderrCapture captured;
    ASSERT_TRUE(captured.ok);
    assert_rejects(file.path);
    // The note must come from the pre-screen, not from Assimp failing later: the
    // importer's own size check is the one the overflow defeats.
    ASSERT_TRUE(captured.text().find("declared terrain grid exceeds") != std::string::npos);
}

TEST(assimp, terragen_truncated_altw_fails_in_the_prescreen)
{
    std::string out = "TERRAGENTERRAIN SIZE";
    append_i16_le(out, 1);
    out.append(2, '\0');
    out += "ALTW";
    TmpFile file(tmp_path("rast_assimp_truncated_altw.ter"), out);
    StderrCapture captured;
    ASSERT_TRUE(captured.ok);
    assert_rejects(file.path);
    ASSERT_TRUE(captured.text().find("declared terrain grid exceeds") != std::string::npos);
}

TEST(assimp, terragen_declared_grid_loads)
{
    TmpFile file(tmp_path("rast_assimp_xpts.ter"), terragen_declared_grid());
    const Mesh mesh = load_ok(file.path);
    ASSERT_FALSE(mesh.triangles.empty());
}

// Motion-only formats must not become synthetic stick figures.
TEST(assimp, bvh_motion_only_fails_instead_of_synthesizing_geometry)
{
    TmpFile file(tmp_path("rast_assimp_motion.bvh"), kBvhMotionOnly);
    assert_rejects(file.path);
}

// DXF's default gray repeats 0.6 in alpha, but does not author opacity.
TEST(assimp, dxf_default_color_alpha_does_not_enable_blending)
{
    TmpFile file(tmp_path("rast_assimp_face.dxf"), kDxfDefaultColorFace);
    const Mesh mesh = load_ok(file.path);
    const Material &material = mesh.mat_at(mesh.triangles.front().material_idx);
    ASSERT_NEAR(material.alpha, 1.0f, 1e-5f);
    ASSERT_FALSE(material.blend);
    ASSERT_FALSE(mesh.has_transparent);
    ASSERT_TRUE(mesh.has_vertex_colors);
    ASSERT_FALSE(mesh.has_vertex_alpha);
}

// RAW names textures but emits no UVs, so bindings must be dropped.
TEST(assimp, raw_texture_slot_without_uvs_is_dropped)
{
    TmpFile texture(tmp_path("rast_assimp_raw_tex.bmp"), k1x1_red_bmp, sizeof(k1x1_red_bmp));
    (void)texture;
    TmpFile file(tmp_path("rast_assimp_lines.raw"), "rast_assimp_raw_tex.bmp\n0 0 0 1 0 0 0 1 0\n");
    const Mesh mesh = load_ok(file.path);
    const Material &material = mesh.mat_at(mesh.triangles.front().material_idx);
    ASSERT_EQ(material.diffuse_map.tex, -1);
    ASSERT_TRUE(mesh.textures.empty());
}

// Assimp reports X3D repeatS=true as Clamp.
TEST(assimp, x3d_repeat_bools_map_to_repeat_wrap)
{
    TmpFile texture(tmp_path("rast_assimp_red.bmp"), k1x1_red_bmp, sizeof(k1x1_red_bmp));
    (void)texture;
    TmpFile file(tmp_path("rast_assimp_box.x3d"), kX3dTexturedBox);
    const Mesh mesh = load_ok(file.path);
    const Material &material = mesh.mat_at(mesh.triangles.front().material_idx);
    ASSERT_TRUE(material.diffuse_map.tex >= 0);
    const Texture &texture_loaded = mesh.textures[static_cast<size_t>(material.diffuse_map.tex)];
    ASSERT_EQ(texture_loaded.wrap_s, WrapMode::Repeat);
    ASSERT_EQ(texture_loaded.wrap_t, WrapMode::Repeat);
}

// Assimp joins X3D fallback URLs. The first URL must still resolve.
TEST(assimp, x3d_multi_url_takes_the_first)
{
    TmpFile texture(tmp_path("rast_assimp_multi.bmp"), k1x1_red_bmp, sizeof(k1x1_red_bmp));
    (void)texture;
    TmpFile file(tmp_path("rast_assimp_multi.x3d"), R"(<?xml version="1.0" encoding="UTF-8"?>
<X3D profile="Interchange">
<Scene>
<Shape>
<Appearance>
<ImageTexture url='"rast_assimp_multi.bmp" "rast_assimp_missing.png"'/>
<Material diffuseColor="1 0 0"/>
</Appearance>
<Box size="1 1 1"/>
</Shape>
</Scene>
</X3D>)");
    const Mesh mesh = load_ok(file.path);
    const Material &material = mesh.mat_at(mesh.triangles.front().material_idx);
    ASSERT_TRUE(material.diffuse_map.tex >= 0);
}

// Convert grayscale Collada <bump> data, but keep colored normal maps.
TEST(assimp, collada_grayscale_bump_converts_to_normals)
{
    TmpFile bump(tmp_path("rast_assimp_height.bmp"), k1x1_gray_bmp, sizeof(k1x1_gray_bmp));
    (void)bump;
    TmpFile file(tmp_path("rast_assimp_bump.dae"), kBumpCollada);
    const Mesh mesh = load_ok(file.path);
    const Material &material = mesh.mat_at(mesh.triangles.front().material_idx);
    ASSERT_TRUE(material.normal_map.tex >= 0);
    const Texture &normal = mesh.textures[static_cast<size_t>(material.normal_map.tex)];
    ASSERT_TRUE(normal.pixels[2] > normal.pixels[0]);
}

TEST(assimp, collada_shared_diffuse_and_bump_decode_separately)
{
    TmpFile image(tmp_path("rast_assimp_height.bmp"), k1x1_gray_bmp, sizeof(k1x1_gray_bmp));
    std::string dae = kBumpCollada;
    const std::string diffuse = "<diffuse><color>1 1 1 1</color></diffuse>";
    const size_t pos = dae.find(diffuse);
    ASSERT_TRUE(pos != std::string::npos);
    dae.replace(pos, diffuse.size(), R"(<diffuse><texture texture="bumpsampler" texcoord="CHANNEL0"/></diffuse>)");
    TmpFile file(tmp_path("rast_assimp_shared_roles.dae"), dae);
    const Mesh mesh = load_ok(file.path);
    const Material &material = mesh.mat_at(mesh.triangles.front().material_idx);
    ASSERT_TRUE(material.diffuse_map.tex >= 0);
    ASSERT_TRUE(material.normal_map.tex >= 0);
    ASSERT_TRUE(material.diffuse_map.tex != material.normal_map.tex);
    const Texture &diffuse_texture = mesh.textures[static_cast<size_t>(material.diffuse_map.tex)];
    const Texture &normal_texture = mesh.textures[static_cast<size_t>(material.normal_map.tex)];
    ASSERT_EQ(diffuse_texture.pixels[2], uint8_t{ 128 });
    ASSERT_TRUE(normal_texture.pixels[2] > normal_texture.pixels[0]);
}

// Ignore Collada's template shininess of 10.
TEST(assimp, collada_template_shininess_is_treated_as_absent)
{
    TmpFile file(tmp_path("rast_assimp_template_shininess.dae"), kPhongNoShininessCollada);
    const Mesh mesh = load_ok(file.path);
    const Material &material = mesh.mat_at(mesh.triangles.front().material_idx);
    ASSERT_NEAR(material.shininess, 32.0f, 1e-4f);
}

// Keep authored Collada shininess.
TEST(assimp, collada_authored_shininess_survives)
{
    TmpFile file(tmp_path("rast_assimp_phong.dae"), kPhongCollada);
    const Mesh mesh = load_ok(file.path);
    const Material &material = mesh.mat_at(mesh.triangles.front().material_idx);
    ASSERT_NEAR(material.shininess, 80.0f, 1e-4f);
}

// Authored FBX shininess must beat its derived roughness.
TEST(assimp, fbx_authored_shininess_survives_derived_roughness)
{
    TmpFile file(tmp_path("rast_assimp_phong.fbx"), kFbxPhongShininess);
    const Mesh mesh = load_ok(file.path);
    const Material &material = mesh.mat_at(mesh.triangles.front().material_idx);
    ASSERT_NEAR(material.shininess, 30.0f, 1e-4f);
}

// SpecularFactor lands in SHININESS_STRENGTH and multiplies the specular color.
TEST(assimp, fbx_specular_factor_scales_specular_color)
{
    TmpFile file(tmp_path("rast_assimp_phong.fbx"), kFbxPhongShininess);
    const Mesh mesh = load_ok(file.path);
    const Material &material = mesh.mat_at(mesh.triangles.front().material_idx);
    ASSERT_NEAR(material.specular.x, 0.4f, 1e-4f);
    ASSERT_NEAR(material.specular.y, 0.4f, 1e-4f);
    ASSERT_NEAR(material.specular.z, 0.4f, 1e-4f);
}

// FBX's zero ambient default must fall back to diffuse.
TEST(assimp, fbx_zero_ambient_falls_back_to_diffuse)
{
    TmpFile file(tmp_path("rast_assimp_ambient.fbx"), kFbxPhongShininess);
    const Mesh mesh = load_ok(file.path);
    const Material &material = mesh.mat_at(mesh.triangles.front().material_idx);
    ASSERT_NEAR(material.ambient.x, 0.5f, 1e-4f);
    ASSERT_NEAR(material.ambient.y, 0.5f, 1e-4f);
    ASSERT_NEAR(material.ambient.z, 0.5f, 1e-4f);
}

// Keep authored FBX ambient.
TEST(assimp, fbx_authored_ambient_is_kept)
{
    TmpFile file(tmp_path("rast_assimp_ambient2.fbx"), kFbxAuthoredAmbient);
    const Mesh mesh = load_ok(file.path);
    const Material &material = mesh.mat_at(mesh.triangles.front().material_idx);
    ASSERT_NEAR(material.ambient.x, 0.1f, 1e-4f);
    ASSERT_NEAR(material.ambient.y, 0.2f, 1e-4f);
    ASSERT_NEAR(material.ambient.z, 0.3f, 1e-4f);
}

// Reject colored textures routed to HEIGHT by Assimp's filename heuristic.
TEST(assimp, x_colored_height_texture_is_dropped)
{
    TmpFile diffuse(tmp_path("rast_assimp_red.bmp"), k1x1_red_bmp, sizeof(k1x1_red_bmp));
    (void)diffuse;
    TmpFile bump(tmp_path("rast_assimp_bump.bmp"), k1x1_gray_bmp, sizeof(k1x1_gray_bmp));
    (void)bump;
    TmpFile file(
        tmp_path("rast_assimp_bump.x"), textured_x_with_height("rast_assimp_red.bmp", "rast_assimp_colored_height.bmp")
    );
    const Mesh mesh = load_ok(file.path);
    const Material &material = mesh.mat_at(mesh.triangles.front().material_idx);
    ASSERT_EQ(material.normal_map.tex, -1);
}

// Flat gray height data becomes a neutral normal.
TEST(assimp, x_grayscale_height_texture_converts_to_normals)
{
    TmpFile diffuse(tmp_path("rast_assimp_red.bmp"), k1x1_red_bmp, sizeof(k1x1_red_bmp));
    (void)diffuse;
    TmpFile bump(tmp_path("rast_assimp_height.bmp"), k1x1_gray_bmp, sizeof(k1x1_gray_bmp));
    (void)bump;
    TmpFile file(
        tmp_path("rast_assimp_height.x"), textured_x_with_height("rast_assimp_red.bmp", "rast_assimp_height.bmp")
    );
    const Mesh mesh = load_ok(file.path);
    const Material &material = mesh.mat_at(mesh.triangles.front().material_idx);
    ASSERT_TRUE(material.normal_map.tex >= 0);
    const Texture &normal = mesh.textures[static_cast<size_t>(material.normal_map.tex)];
    ASSERT_TRUE(normal.pixels[2] > normal.pixels[0]);
}

// Assimp's prefix lookup can write a longer Blender property through one int slot.
TEST(assimp, blend_func_ignores_a_longer_key_sharing_its_prefix)
{
    aiMaterial material;
    const aiColor3D diffuse(0.25f, 0.5f, 0.75f);
    material.AddProperty(&diffuse, 1, "$mat.blend.diffuse.color", 0, 0);
    int blend_func = -1;
    ASSERT_TRUE(!assimp_detail::get_blend_func(material, blend_func));
    ASSERT_EQ(blend_func, -1);
}

// Exact lookup must walk past longer prefix matches.
TEST(assimp, blend_func_is_found_behind_a_longer_key_sharing_its_prefix)
{
    aiMaterial material;
    const aiColor3D diffuse(0.25f, 0.5f, 0.75f);
    material.AddProperty(&diffuse, 1, "$mat.blend.diffuse.color", 0, 0);
    const int mode = static_cast<int>(aiBlendMode_Additive);
    material.AddProperty(&mode, 1, AI_MATKEY_BLEND_FUNC);
    int blend_func = -1;
    ASSERT_TRUE(assimp_detail::get_blend_func(material, blend_func));
    ASSERT_EQ(blend_func, static_cast<int>(aiBlendMode_Additive));
}

TEST(assimp, blend_func_is_absent_when_no_material_key_carries_it)
{
    aiMaterial material;
    int blend_func = -1;
    ASSERT_TRUE(!assimp_detail::get_blend_func(material, blend_func));
    ASSERT_EQ(blend_func, -1);
}

// Some malformed files assert immediately before Assimp throws. The adapter replaces
// that debug-only abort. Avoid the leaking 3MF case in this sanitizer test.
TEST(assimp, an_assert_violation_reports_instead_of_aborting)
{
    // A supported extension installs the handler before this missing file fails.
    assert_rejects(tmp_path("rast_assimp_absent.dae"));
#ifdef ASSIMP_BUILD_DEBUG
    // The default debug handler would abort here.
    Assimp::aiAssertViolation("rasterminal test violation", __FILE__, __LINE__);
#endif
}

// A corrupt animation sidecar must not fail a static MD5 mesh.
TEST(assimp, md5mesh_loads_despite_a_corrupt_anim_sidecar)
{
    TmpFile mesh_file(tmp_path("rast_assimp_dude.md5mesh"), kMd5MeshTriangle);
    TmpFile corrupt_anim(tmp_path("rast_assimp_dude.md5anim"), "MD5Version 10\ngarbage {\n");
    const Mesh mesh = load_ok(mesh_file.path);
    ASSERT_EQ(mesh.triangles.size(), size_t{ 1 });
}

// Ignore MD2's hardcoded specular color.
TEST(assimp, md2_synthetic_specular_is_ignored)
{
    constexpr uint32_t kMagic = 0x32504449u; // "IDP2"
    // header | skin name (64) | 3 texcoords | 1 triangle | frame (scale+translate+name+3 verts)
    struct Header
    {
        uint32_t magic, version, skin_width, skin_height, frame_size, num_skins, num_vertices, num_texcoords,
            num_triangles, num_glcommands, num_frames, offset_skins, offset_texcoords, offset_triangles,
            offset_glcommands, offset_frames, offset_end;
    };
    Header header{};
    header.magic = kMagic;
    header.version = 8;
    header.skin_width = 4;
    header.skin_height = 4;
    header.num_skins = 1;
    header.num_vertices = 3;
    header.num_texcoords = 3;
    header.num_triangles = 1;
    header.num_glcommands = 0;
    header.num_frames = 1;
    size_t offset = sizeof(Header);
    header.offset_skins = static_cast<uint32_t>(offset);
    offset += 64; // one Skin
    header.offset_texcoords = static_cast<uint32_t>(offset);
    offset += 3ULL * 4; // TexCoord[s,t] u16 pairs
    header.offset_triangles = static_cast<uint32_t>(offset);
    offset += 12; // Triangle: 6 u16 indices
    header.offset_frames = static_cast<uint32_t>(offset);
    offset += 40 + (3 * 4); // Frame header + 3 vertices
    header.offset_glcommands = static_cast<uint32_t>(offset);
    header.frame_size = 40 + (3 * 4);
    header.offset_end = static_cast<uint32_t>(offset);

    // The importer's validation demands every region end strictly before EOF, so pad.
    std::vector<uint8_t> bytes(offset + 128, 0);
    std::memcpy(bytes.data(), &header, sizeof header);
    std::memcpy(bytes.data() + header.offset_skins, "rast_assimp_absent.bmp", 22);

    const uint16_t texcoords[3][2] = { { 0, 0 }, { 4, 0 }, { 0, 4 } };
    std::memcpy(bytes.data() + header.offset_texcoords, texcoords, sizeof texcoords);
    const uint16_t triangle[6] = { 0, 1, 2, 0, 1, 2 };
    std::memcpy(bytes.data() + header.offset_triangles, triangle, sizeof triangle);
    float frame_header[6] = { 1.0f, 1.0f, 1.0f, 0.0f, 0.0f, 0.0f };
    std::memcpy(bytes.data() + header.offset_frames, frame_header, sizeof frame_header);
    std::memcpy(bytes.data() + header.offset_frames + 24, "frame0", 6);
    const uint8_t vertices[3][4] = { { 0, 0, 0, 0 }, { 10, 0, 0, 0 }, { 0, 10, 0, 0 } };
    std::memcpy(bytes.data() + header.offset_frames + 40, vertices, sizeof vertices);

    TmpFile file(tmp_path("rast_assimp_tris.md2"), bytes.data(), bytes.size());
    const Mesh mesh = load_ok(file.path);
    ASSERT_EQ(mesh.triangles.size(), size_t{ 1 });
    const Material &material = mesh.mat_at(mesh.triangles.front().material_idx);
    ASSERT_NEAR(material.diffuse.x, 1.0f, 1e-5f);
    ASSERT_NEAR(material.specular.x, 0.4f, 1e-5f);
    ASSERT_NEAR(material.specular.y, 0.4f, 1e-5f);
}

// Ignore DXF's synthetic 0.05 gray ambient.
TEST(assimp, dxf_synthetic_ambient_is_ignored)
{
    TmpFile file(tmp_path("rast_assimp_face.dxf"), kDxfDefaultColorFace);
    const Mesh mesh = load_ok(file.path);
    const Material &material = mesh.mat_at(mesh.triangles.front().material_idx);
    ASSERT_NEAR(material.ambient.x, material.diffuse.x, 1e-5f);
    ASSERT_NEAR(material.ambient.y, material.diffuse.y, 1e-5f);
}

// Replicate NFF's scalar Ka from red.
TEST(assimp, nff_scalar_ambient_replicates_to_grey)
{
    TmpFile file(tmp_path("rast_assimp_ka.nff"), "f 1 0 0 0.5 0.1 10 0 1 0.2\np 3\n0 0 0\n1 0 0\n0 1 0\n");
    const Mesh mesh = load_ok(file.path);
    const Material &material = mesh.mat_at(mesh.triangles.front().material_idx);
    ASSERT_NEAR(material.ambient.x, 0.2f, 1e-5f);
    ASSERT_NEAR(material.ambient.y, 0.2f, 1e-5f);
    ASSERT_NEAR(material.ambient.z, 0.2f, 1e-5f);
}

// NFF tessellation grows without an upstream bound.
TEST(assimp, nff_oversized_tessellation_fails_the_prescreen)
{
    StderrCapture captured;
    ASSERT_TRUE(captured.ok);
    TmpFile file(tmp_path("rast_assimp_tess.nff"), "tess 13\ns 0 0 0 1\n");
    assert_rejects(file.path);
    ASSERT_TRUE(captured.text().find("tessellation") != std::string::npos);

    TmpFile sane(tmp_path("rast_assimp_tess_sane.nff"), "tess 3\ns 0 0 0 1\n");
    const Mesh mesh = load_ok(sane.path);
    (void)mesh;
}

TEST(assimp, nff_overlong_line_does_not_hide_tessellation)
{
    StderrCapture captured;
    ASSERT_TRUE(captured.ok);
    std::string contents(size_t{ 70 } * 1024, ' ');
    contents += "\ntess 9\ns 0 0 0 1\n";
    TmpFile file(tmp_path("rast_assimp_tess_long.nff"), contents);
    assert_rejects(file.path);
    ASSERT_TRUE(captured.text().find("tessellation") != std::string::npos);
}

TEST(assimp, nff_long_irrelevant_line_is_accepted)
{
    std::string contents(size_t{ 70 } * 1024, ' ');
    contents += "\ntess 3\ns 0 0 0 1\n";
    TmpFile file(tmp_path("rast_assimp_tess_long_sane.nff"), contents);
    const Mesh mesh = load_ok(file.path);
    ASSERT_FALSE(mesh.triangles.empty());
}

TEST(assimp, nff_cr_line_endings_do_not_hide_tessellation)
{
    StderrCapture captured;
    ASSERT_TRUE(captured.ok);
    TmpFile file(tmp_path("rast_assimp_tess_cr.nff"), "tess 9\rs 0 0 0 1\r");
    assert_rejects(file.path);
    ASSERT_TRUE(captured.text().find("tessellation") != std::string::npos);
}

TEST(assimp, nff_importer_chunk_boundary_does_not_hide_tessellation)
{
    StderrCapture captured;
    ASSERT_TRUE(captured.ok);
    std::string contents = "#" + std::string(size_t{ 4094 }, 'x') + "tess 9\ns 0 0 0 1\n";
    TmpFile file(tmp_path("rast_assimp_tess_chunk.nff"), contents);
    assert_rejects(file.path);
    ASSERT_TRUE(captured.text().find("tessellation") != std::string::npos);
}

// Normalize integer COFF alpha with RGB.
TEST(assimp, off_integer_alpha_normalizes)
{
    TmpFile file(
        tmp_path("rast_assimp_alpha.off"),
        "COFF\n3 1 0\n0 0 0 255 0 0 64\n1 0 0 255 0 0 64\n0 1 0 255 0 0 64\n3 0 1 2 0 0 0 255\n"
    );
    const Mesh mesh = load_ok(file.path);
    ASSERT_TRUE(mesh.has_vertex_alpha);
    for (const float alpha : mesh.vertex_alpha)
    {
        ASSERT_NEAR(alpha, 64.0f / 255.0f, 1e-4f);
    }
}

// Terragen's generated normals precede the winding reversal.
TEST(assimp, terragen_normals_point_up_after_the_winding_reversal)
{
    TmpFile file(tmp_path("rast_assimp_normals.ter"), terragen_declared_grid());
    const Mesh mesh = load_ok(file.path);
    ASSERT_FALSE(mesh.triangles.empty());
    for (const Vertex &vertex : mesh.vertices)
    {
        ASSERT_TRUE(vertex.normal.y > 0.5f);
    }
}

// A later SIZE chunk can reintroduce Terragen's overflow.
TEST(assimp, terragen_oversized_grid_after_altw_fails_the_prescreen)
{
    StderrCapture captured;
    ASSERT_TRUE(captured.ok);
    std::string out = "TERRAGENTERRAIN ";
    out += "XPTS";
    append_i16_le(out, 2);
    out.append(2, '\0');
    out += "YPTS";
    append_i16_le(out, 2);
    out.append(2, '\0');
    out += "ALTW";
    append_i16_le(out, 0);
    append_i16_le(out, 0);
    out.append(2ULL * 2 * 2, '\0');
    out += "SIZE";
    append_i16_le(out, static_cast<int16_t>(0xFFFF));
    out.append(2, '\0');
    out += "ALTW";
    append_i16_le(out, 0);
    append_i16_le(out, 0);
    TmpFile file(tmp_path("rast_assimp_rearm.ter"), out);
    assert_rejects(file.path);
    ASSERT_TRUE(captured.text().find("terrain grid") != std::string::npos);
}

// Validate Unreal indices against vertices and step over 16-byte records.
TEST(assimp, unreal_out_of_range_triangle_index_fails_the_prescreen)
{
    StderrCapture captured;
    ASSERT_TRUE(captured.ok);
    std::string d;
    auto append_u16 = [&d](unsigned v)
    {
        d.push_back(static_cast<char>(v & 0xFF));
        d.push_back(static_cast<char>((v >> 8u) & 0xFF));
    };
    append_u16(2); // numTris
    append_u16(4); // numVert
    d.append(44, '\0');
    for (unsigned tri = 0; tri < 2; tri++)
    {
        append_u16(tri == 1 ? 9 : 0); // second triangle: index 9 past numVert
        append_u16(1);
        append_u16(2);
        d.append(10, '\0'); // type, color, texcoords, texnum, flags
    }
    TmpFile file(tmp_path("rast_assimp_u_d.3d"), d);
    assert_rejects(file.path);
    ASSERT_TRUE(captured.text().find("vertex index") != std::string::npos);
}

// Irrlicht's RGB-only colors leave alpha at zero.
TEST(assimp, irrmesh_six_digit_hex_colors_stay_opaque)
{
    TmpFile file(
        tmp_path("rast_assimp_hex.irrmesh"),
        R"(<?xml version="1.0"?>
<mesh>
  <buffer>
    <material/>
    <vertices vertexCount="3" type="standard">
0 0 0 0 0 1 ff0000 0 0
1 0 0 0 0 1 00ff00 1 0
0 1 0 0 0 1 ff0000 0 1</vertices>
    <indices indexCount="3">0 1 2</indices>
  </buffer>
</mesh>)"
    );
    const Mesh mesh = load_ok(file.path);
    ASSERT_TRUE(mesh.has_vertex_colors);
    ASSERT_FALSE(mesh.has_vertex_alpha);
    ASSERT_FALSE(mesh.has_transparent);
}

// Recover OpenGEX's dropped up metric. Z is the default.
constexpr const char *kOgexZupTriangle = R"(Metric (key = "distance") {float {1}}
Metric (key = "angle") {float {1}}
Metric (key = "time") {float {1}}
Metric (key = "up") {string {"z"}}

GeometryNode $node1
{
	Name {string {"Tri"}}
	ObjectRef {ref {$geometry1}}
	MaterialRef {ref {$material1}}

	Transform
	{
		float[16]
		{
			{0x3F800000, 0x00000000, 0x00000000, 0x00000000,
			 0x00000000, 0x3F800000, 0x00000000, 0x00000000,
			 0x00000000, 0x00000000, 0x3F800000, 0x00000000,
			 0xBEF33B00, 0x411804DE, 0x00000000, 0x3F800000}
		}
	}
}

GeometryObject $geometry1
{
	Mesh (primitive = "triangles")
	{
		VertexArray (attrib = "position")
		{
			float[3]
			{
				{0.0, 0.0, 0.0}, {1.0, 0.0, 0.0}, {0.0, 0.0, 2.0}
			}
		}

		IndexArray
		{
			unsigned_int32[3]
			{
				{0, 1, 2}
			}
		}
	}
}

Material $material1
{
	Name {string {"mat"}}
	Color (attrib = "diffuse") {float[3] {{1.0, 1.0, 1.0}}}
}
)";

TEST(assimp, ogex_zup_default_is_remapped_to_yup)
{
    TmpFile file(tmp_path("rast_assimp_zup.ogex"), kOgexZupTriangle);
    const Mesh mesh = load_ok(file.path);
    vec3 lo, hi;
    bounds(mesh, lo, hi);
    // The 2-unit source elevation moves from z to y.
    ASSERT_NEAR(hi.y - lo.y, 2.0f, 1e-5f);
    ASSERT_TRUE(hi.z - lo.z < 0.5f);
}

TEST(assimp, ogex_declared_yup_stays_put)
{
    std::string yup = kOgexZupTriangle;
    const size_t value = yup.find("\"z\"}}");
    ASSERT_TRUE(value != std::string::npos);
    yup.replace(value, 5, "\"y\"}}");
    TmpFile file(tmp_path("rast_assimp_yup.ogex"), yup);
    const Mesh mesh = load_ok(file.path);
    vec3 lo, hi;
    bounds(mesh, lo, hi);
    // No remap: the elevation stays on z (the node translation offsets both).
    ASSERT_NEAR(hi.z - lo.z, 2.0f, 1e-5f);
    ASSERT_TRUE(hi.y - lo.y < 0.5f);
}

TEST(assimp, ogex_finds_up_axis_after_a_long_comment)
{
    std::string yup = "/*" + std::string(size_t{ 70 } * 1024, 'x') + "*/\n" + kOgexZupTriangle;
    const size_t value = yup.find("\"z\"}}");
    ASSERT_TRUE(value != std::string::npos);
    yup.replace(value, 5, "\"y\"}}");
    TmpFile file(tmp_path("rast_assimp_yup_long_comment.ogex"), yup);
    const Mesh mesh = load_ok(file.path);
    vec3 lo, hi;
    bounds(mesh, lo, hi);
    ASSERT_NEAR(hi.z - lo.z, 2.0f, 1e-5f);
    ASSERT_TRUE(hi.y - lo.y < 0.5f);
}

TEST(assimp, ogex_ignores_metrics_inside_comments)
{
    std::string yup = kOgexZupTriangle;
    const size_t value = yup.find("\"z\"}}");
    ASSERT_TRUE(value != std::string::npos);
    yup.replace(value, 5, "\"y\"}}");
    yup = "/* Metric (key = \"up\") {string {\"z\"}} */\n" + yup;
    const size_t metric = yup.find("Metric (key", yup.find("*/"));
    ASSERT_TRUE(metric != std::string::npos);
    yup.insert(metric + 6, "/*" + std::string(size_t{ 2 } * 1024, 'x') + "*/");
    TmpFile file(tmp_path("rast_assimp_yup_commented_metric.ogex"), yup);
    const Mesh mesh = load_ok(file.path);
    vec3 lo, hi;
    bounds(mesh, lo, hi);
    ASSERT_NEAR(hi.z - lo.z, 2.0f, 1e-5f);
    ASSERT_TRUE(hi.y - lo.y < 0.5f);
}

// Reject OFF counts that cannot fit in the file.
TEST(assimp, off_oversized_declared_counts_fail_the_prescreen)
{
    StderrCapture captured;
    ASSERT_TRUE(captured.ok);
    TmpFile file(tmp_path("rast_assimp_counts.off"), "OFF\n999999999 1 0\n");
    assert_rejects(file.path);
    ASSERT_TRUE(captured.text().find("vertex or face count") != std::string::npos);

    TmpFile sane(
        tmp_path("rast_assimp_counts_sane.off"),
        "COFF\n3 1 0\n0 0 0 255 0 0 255\n1 0 0 255 0 0 255\n0 1 0 255 0 0 255\n3 0 1 2 0 0 0 255\n"
    );
    const Mesh mesh = load_ok(sane.path);
    (void)mesh;
}

TEST(assimp, off_comment_does_not_hide_declared_counts)
{
    StderrCapture captured;
    ASSERT_TRUE(captured.ok);
    TmpFile file(tmp_path("rast_assimp_counts_comment.off"), "OFF\n# generated model\n999999999 1 0\n");
    assert_rejects(file.path);
    ASSERT_TRUE(captured.text().find("vertex or face count") != std::string::npos);
}

TEST(assimp, off_cr_comment_does_not_hide_declared_counts)
{
    StderrCapture captured;
    ASSERT_TRUE(captured.ok);
    TmpFile file(tmp_path("rast_assimp_counts_cr_comment.off"), "OFF\r# generated model\r999999999 1 0\r");
    assert_rejects(file.path);
    ASSERT_TRUE(captured.text().find("vertex or face count") != std::string::npos);
}

TEST(assimp, noff_dimension_precedes_declared_counts)
{
    StderrCapture captured;
    ASSERT_TRUE(captured.ok);
    TmpFile file(tmp_path("rast_assimp_counts_noff.off"), "nOFF\n3\n3 999999999 0\n");
    assert_rejects(file.path);

    TmpFile combined(tmp_path("rast_assimp_counts_cnoff.off"), "CnOFF\n3\n3 999999999 0\n");
    assert_rejects(combined.path);
    const std::string output = captured.text();
    ASSERT_TRUE(output.find("vertex or face count in '" + file.path + "'") != std::string::npos);
    ASSERT_TRUE(output.find("vertex or face count in '" + combined.path + "'") != std::string::npos);
}

TEST(assimp, off_count_arithmetic_cannot_wrap)
{
    StderrCapture captured;
    ASSERT_TRUE(captured.ok);
    TmpFile file(tmp_path("rast_assimp_counts_wrap.off"), "OFF\n3074457345618258603 1 0\n");
    assert_rejects(file.path);
    ASSERT_TRUE(captured.text().find("vertex or face count") != std::string::npos);
}

TEST(assimp, ac_zup_geometry_is_remapped_to_yup)
{
    TmpFile file(
        tmp_path("rast_assimp_zup.ac"),
        "AC3Db\nMATERIAL \"mat\" rgb 1 1 1 amb 0.2 0.2 0.2 emis 0 0 0 spec 0 0 0 shi 0 trans 0\n"
        "OBJECT world\nkids 1\nOBJECT poly\nmat 0\nnumvert 3\n"
        "0 0 0\n1 0 0\n0 -1 2\nnumsurf 1\nSURF 0x10\nmat 0\nrefs 3\n0 0 0\n1 0 0\n2 0 0\nkids 0\n"
    );
    const Mesh mesh = load_ok(file.path);
    vec3 lo, hi;
    bounds(mesh, lo, hi);
    // Source elevation on +Z lands on +Y.
    ASSERT_NEAR(hi.y - lo.y, 2.0f, 1e-5f);
}

// Ignore X3D's default ambientIntensity of 0.2.
TEST(assimp, x3d_default_ambient_is_ignored)
{
    TmpFile file(
        tmp_path("rast_assimp_mat.x3d"),
        R"(<?xml version="1.0" encoding="UTF-8"?>
<X3D profile="interchange">
<Scene>
<Shape>
<Appearance><Material diffuseColor="0.8 0.6 0.4"/></Appearance>
<Box/>
</Shape>
</Scene>
</X3D>)"
    );
    const Mesh mesh = load_ok(file.path);
    const Material &material = mesh.mat_at(mesh.triangles.front().material_idx);
    ASSERT_NEAR(material.ambient.x, material.diffuse.x, 1e-5f);
}

// Screen AMF texture dimensions before Assimp's 32-bit product can wrap.
TEST(assimp, amf_oversized_declared_texture_fails_the_prescreen)
{
    StderrCapture captured;
    ASSERT_TRUE(captured.ok);
    TmpFile file(
        tmp_path("rast_assimp_bigtex.amf"),
        "<?xml version=\"1.0\"?>\n<amf unit=\"millimeter\">\n"
        "<texture id=\"t\" type=\"grayscale\" width=\"65536\" height=\"65536\" depth=\"2\">AA==</texture>\n"
        "</amf>\n"
    );
    assert_rejects(file.path);

    TmpFile single_quotes(
        tmp_path("rast_assimp_bigtex_single_quotes.amf"),
        "<amf><texture id='t' type='grayscale' width='65536' height='65536' depth='2'>AA==</texture></amf>"
    );
    assert_rejects(single_quotes.path);

    TmpFile missing_depth(
        tmp_path("rast_assimp_bigtex_missing_depth.amf"),
        R"(<amf><texture id="t" type="grayscale" width="65536" height="65536">AA==</texture></amf>)"
    );
    assert_rejects(missing_depth.path);

    TmpFile quoted_close(
        tmp_path("rast_assimp_bigtex_quoted_close.amf"),
        "<amf><texture id=\"x>y\" type=\"grayscale\" width=\"65536\" height=\"65536\" "
        "depth=\"2\">AA==</texture></amf>"
    );
    assert_rejects(quoted_close.path);

    TmpFile misleading_value(
        tmp_path("rast_assimp_bigtex_misleading_value.amf"),
        "<amf><texture id=\"width='1' height='1' depth='1'\" type=\"grayscale\" width=\"65536\" "
        "height=\"65536\" depth=\"2\">AA==</texture></amf>"
    );
    assert_rejects(misleading_value.path);

    TmpFile escaped_dimensions(
        tmp_path("rast_assimp_bigtex_escaped_dimensions.amf"),
        "<amf><texture id=\"t\" type=\"grayscale\" width=\"&#54;5536\" height=\"65536\" "
        "depth=\"2\">AA==</texture></amf>"
    );
    assert_rejects(escaped_dimensions.path);

    const std::string output = captured.text();
    ASSERT_TRUE(output.find("declared texture size in '" + file.path + "'") != std::string::npos);
    ASSERT_TRUE(output.find("declared texture size in '" + single_quotes.path + "'") != std::string::npos);
    ASSERT_TRUE(output.find("declared texture size in '" + missing_depth.path + "'") != std::string::npos);
    ASSERT_TRUE(output.find("declared texture size in '" + quoted_close.path + "'") != std::string::npos);
    ASSERT_TRUE(output.find("declared texture size in '" + misleading_value.path + "'") != std::string::npos);
    ASSERT_TRUE(output.find("declared texture size in '" + escaped_dimensions.path + "'") != std::string::npos);

    TmpFile sane(tmp_path("rast_assimp_bigtex_sane.amf"), kAmfTexturedTriangle);
    const Mesh mesh = load_ok(sane.path);
    (void)mesh;
}

TEST(assimp, amf_ignores_texture_text_in_comments_and_cdata)
{
    std::string contents = kAmfTexturedTriangle;
    const size_t root_start = contents.find("<amf");
    ASSERT_TRUE(root_start != std::string::npos);
    const size_t root = contents.find('>', root_start);
    ASSERT_TRUE(root != std::string::npos);
    contents.insert(
        root + 1, "<!-- <texture width=\"65536\" height=\"65536\" depth=\"2\"> -->\n"
                  "<![CDATA[<texture width=\"65536\" height=\"65536\" depth=\"2\">]]>\n"
    );
    TmpFile file(tmp_path("rast_assimp_texture_markup.amf"), contents);
    const Mesh mesh = load_ok(file.path);
    ASSERT_EQ(mesh.textures.size(), size_t{ 1 });
}

TEST(assimp, amf_accepts_a_long_texture_opening_tag)
{
    std::string contents = kAmfTexturedTriangle;
    const size_t width = contents.find("width=\"1\"");
    ASSERT_TRUE(width != std::string::npos);
    contents.insert(width, "note=\"" + std::string(size_t{ 5 } * 1024, 'x') + "\" ");
    TmpFile file(tmp_path("rast_assimp_long_texture_tag.amf"), contents);
    const Mesh mesh = load_ok(file.path);
    ASSERT_EQ(mesh.textures.size(), size_t{ 1 });
}

TEST(assimp, amf_scans_texture_after_a_long_prefix)
{
    StderrCapture captured;
    ASSERT_TRUE(captured.ok);
    std::string contents = "<?xml version=\"1.0\"?>\n<amf unit=\"millimeter\">\n<!--";
    contents.append((size_t{ 8 } * 1024 * 1024) + 1, 'x');
    contents += "-->\n<texture id=\"t\" type=\"grayscale\" width=\"65536\" height=\"65536\" "
                "depth=\"2\">AA==</texture>\n</amf>\n";
    TmpFile file(tmp_path("rast_assimp_bigtex_late.amf"), contents);
    assert_rejects(file.path);
    ASSERT_TRUE(captured.text().find("texture size") != std::string::npos);
}

// Reject MD5 counts that cannot fit in the file.
TEST(assimp, md5mesh_oversized_declared_counts_fail_the_prescreen)
{
    StderrCapture captured;
    ASSERT_TRUE(captured.ok);
    TmpFile file(
        tmp_path("rast_assimp_big.md5mesh"),
        "MD5Version 10\nnumJoints 1\nnumMeshes 1\njoints {\n\"o\" -1 ( 0 0 0 ) ( 0 0 0 )\n}\n"
        "mesh {\nnumverts 2000000000\nnumtris 1\ntri 0 0 1 2\n}\n"
    );
    assert_rejects(file.path);
    ASSERT_TRUE(captured.text().find("element counts") != std::string::npos);

    TmpFile sane(tmp_path("rast_assimp_big_sane.md5mesh"), kMd5MeshTriangle);
    const Mesh mesh = load_ok(sane.path);
    ASSERT_EQ(mesh.triangles.size(), size_t{ 1 });
}

TEST(assimp, md5mesh_overlong_line_does_not_hide_counts)
{
    StderrCapture captured;
    ASSERT_TRUE(captured.ok);
    std::string contents = "MD5Version 10\ncommandline \"" + std::string(size_t{ 70 } * 1024, 'x') +
                           "\"\nnumJoints 1\nnumMeshes 1\nnumverts 2000000000\n";
    TmpFile file(tmp_path("rast_assimp_big_long.md5mesh"), contents);
    assert_rejects(file.path);
    ASSERT_TRUE(captured.text().find("element counts") != std::string::npos);
}

TEST(assimp, md5mesh_long_commandline_is_accepted)
{
    std::string contents = kMd5MeshTriangle;
    const size_t command = contents.find("commandline \"\"");
    ASSERT_TRUE(command != std::string::npos);
    contents.replace(command, 14, "commandline \"" + std::string(size_t{ 70 } * 1024, 'x') + "\"");
    TmpFile file(tmp_path("rast_assimp_long_command.md5mesh"), contents);
    const Mesh mesh = load_ok(file.path);
    ASSERT_EQ(mesh.triangles.size(), size_t{ 1 });
}

// AC3D subdivision grows by 4^N.
TEST(assimp, ac_oversized_subdivision_fails_the_prescreen)
{
    StderrCapture captured;
    ASSERT_TRUE(captured.ok);
    TmpFile file(tmp_path("rast_assimp_subdiv.ac"), "AC3Db\nsubdiv 13\nOBJECT world\nkids 0\n");
    assert_rejects(file.path);
    ASSERT_TRUE(captured.text().find("subdivision") != std::string::npos);
}

TEST(assimp, ac_overlong_line_does_not_hide_subdivision)
{
    StderrCapture captured;
    ASSERT_TRUE(captured.ok);
    std::string contents =
        "AC3Db\ndata \"" + std::string(size_t{ 70 } * 1024, 'x') + "\"\nsubdiv 7\nOBJECT world\nkids 0\n";
    TmpFile file(tmp_path("rast_assimp_subdiv_long.ac"), contents);
    assert_rejects(file.path);
    ASSERT_TRUE(captured.text().find("subdivision") != std::string::npos);
}

// Refill zero and non-finite normals left by GenSmoothNormals.
TEST(assimp, zero_normal_vertices_are_refilled_from_adjacent_faces)
{
    TmpFile file(tmp_path("rast_assimp_degenerate.x"), R"(xof 0303txt 0032
Frame Root {
  Mesh TriPlusDegenerate {
    4;
    0;0;0;,
    1;0;0;,
    0;1;0;,
    2;0;0;;
    2;
    3;0,1,2;;
    3;0,1,3;;
  }
})");
    const Mesh mesh = load_ok(file.path);
    ASSERT_EQ(mesh.triangles.size(), size_t{ 2 });
    // An isolated degenerate vertex uses the default normal.
    bool saw_default = false;
    for (const Vertex &vertex : mesh.vertices)
    {
        ASSERT_NEAR(vertex.normal.length(), 1.0f, 1e-5f);
        saw_default = saw_default || std::fabs(vertex.normal.y) > 0.5f;
    }
    ASSERT_TRUE(saw_default);
}
