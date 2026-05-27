// Hand-authored for vendoring. Upstream Draco generates this file from
// cmake/draco_options.cmake based on the enabled feature options. We vendor
// only the glTF-bitstream decoder, so these are the features that build
// produces with DRACO_GLTF_BITSTREAM=ON. Refresh per vendor/README.md.

#ifndef DRACO_FEATURES_H_
#define DRACO_FEATURES_H_

#define DRACO_MESH_COMPRESSION_SUPPORTED
#define DRACO_NORMAL_ENCODING_SUPPORTED
#define DRACO_STANDARD_EDGEBREAKER_SUPPORTED

#endif  // DRACO_FEATURES_H_
