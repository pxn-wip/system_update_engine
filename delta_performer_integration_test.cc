//
// Copyright (C) 2012 The Android Open Source Project
//
// Licensed under the Apache License, Version 2.0 (the "License");
// you may not use this file except in compliance with the License.
// You may obtain a copy of the License at
//
//      http://www.apache.org/licenses/LICENSE-2.0
//
// Unless required by applicable law or agreed to in writing, software
// distributed under the License is distributed on an "AS IS" BASIS,
// WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
// See the License for the specific language governing permissions and
// limitations under the License.
//

#include "update_engine/delta_performer.h"

#include <inttypes.h>
#include <sys/mount.h>

#include <algorithm>
#include <string>
#include <vector>

#include <base/files/file_path.h>
#include <base/files/file_util.h>
#include <base/strings/stringprintf.h>
#include <base/strings/string_util.h>
#include <google/protobuf/repeated_field.h>
#include <gtest/gtest.h>

#include "update_engine/constants.h"
#include "update_engine/fake_hardware.h"
#include "update_engine/fake_system_state.h"
#include "update_engine/mock_prefs.h"
#include "update_engine/payload_constants.h"
#include "update_engine/payload_generator/delta_diff_generator.h"
#include "update_engine/payload_generator/payload_signer.h"
#include "update_engine/payload_verifier.h"
#include "update_engine/test_utils.h"
#include "update_engine/update_metadata.pb.h"
#include "update_engine/utils.h"

namespace chromeos_update_engine {

using std::string;
using std::vector;
using testing::Return;
using testing::_;
using test_utils::kRandomString;
using test_utils::ScopedLoopMounter;
using test_utils::System;

extern const char* kUnittestPrivateKeyPath;
extern const char* kUnittestPublicKeyPath;
extern const char* kUnittestPrivateKey2Path;
extern const char* kUnittestPublicKey2Path;

static const int kDefaultKernelSize = 4096;  // Something small for a test
static const uint8_t kNewData[] = {'T', 'h', 'i', 's', ' ', 'i', 's', ' ',
                                   'n', 'e', 'w', ' ', 'd', 'a', 't', 'a', '.'};

namespace {
struct DeltaState {
  string a_img;
  string b_img;
  string result_img;
  size_t image_size;

  string delta_path;
  uint64_t metadata_size;

  string old_kernel;
  chromeos::Blob old_kernel_data;

  string new_kernel;
  chromeos::Blob new_kernel_data;

  string result_kernel;
  chromeos::Blob result_kernel_data;
  size_t kernel_size;

  // The in-memory copy of delta file.
  chromeos::Blob delta;

  // The mock system state object with which we initialize the
  // delta performer.
  FakeSystemState fake_system_state;
};

enum SignatureTest {
  kSignatureNone,  // No payload signing.
  kSignatureGenerator,  // Sign the payload at generation time.
  kSignatureGenerated,  // Sign the payload after it's generated.
  kSignatureGeneratedPlaceholder,  // Insert placeholder signatures, then real.
  kSignatureGeneratedPlaceholderMismatch,  // Insert a wrong sized placeholder.
  kSignatureGeneratedShell,  // Sign the generated payload through shell cmds.
  kSignatureGeneratedShellBadKey,  // Sign with a bad key through shell cmds.
  kSignatureGeneratedShellRotateCl1,  // Rotate key, test client v1
  kSignatureGeneratedShellRotateCl2,  // Rotate key, test client v2
};

enum OperationHashTest {
  kInvalidOperationData,
  kValidOperationData,
};

}  // namespace

class DeltaPerformerIntegrationTest : public ::testing::Test {
 public:
  static void SetSupportedVersion(DeltaPerformer* performer,
                                  uint64_t minor_version) {
    performer->supported_minor_version_ = minor_version;
  }
};

static void CompareFilesByBlock(const string& a_file, const string& b_file,
                                size_t image_size) {
  EXPECT_EQ(0, image_size % kBlockSize);

  chromeos::Blob a_data, b_data;
  EXPECT_TRUE(utils::ReadFile(a_file, &a_data)) << "file failed: " << a_file;
  EXPECT_TRUE(utils::ReadFile(b_file, &b_data)) << "file failed: " << b_file;

  EXPECT_GE(a_data.size(), image_size);
  EXPECT_GE(b_data.size(), image_size);
  for (size_t i = 0; i < image_size; i += kBlockSize) {
    EXPECT_EQ(0, i % kBlockSize);
    chromeos::Blob a_sub(&a_data[i], &a_data[i + kBlockSize]);
    chromeos::Blob b_sub(&b_data[i], &b_data[i + kBlockSize]);
    EXPECT_TRUE(a_sub == b_sub) << "Block " << (i/kBlockSize) << " differs";
  }
  if (::testing::Test::HasNonfatalFailure()) {
    LOG(INFO) << "Compared filesystems with size " << image_size
              << ", partition A " << a_file << " size: " << a_data.size()
              << ", partition B " << b_file << " size: " << b_data.size();
  }
}

static bool WriteSparseFile(const string& path, off_t size) {
  int fd = open(path.c_str(), O_CREAT | O_TRUNC | O_WRONLY, 0644);
  TEST_AND_RETURN_FALSE_ERRNO(fd >= 0);
  ScopedFdCloser fd_closer(&fd);
  off_t rc = lseek(fd, size + 1, SEEK_SET);
  TEST_AND_RETURN_FALSE_ERRNO(rc != static_cast<off_t>(-1));
  int return_code = ftruncate(fd, size);
  TEST_AND_RETURN_FALSE_ERRNO(return_code == 0);
  return true;
}

static size_t GetSignatureSize(const string& private_key_path) {
  const chromeos::Blob data(1, 'x');
  chromeos::Blob hash;
  EXPECT_TRUE(OmahaHashCalculator::RawHashOfData(data, &hash));
  chromeos::Blob signature;
  EXPECT_TRUE(PayloadSigner::SignHash(hash,
                                      private_key_path,
                                      &signature));
  return signature.size();
}

static bool InsertSignaturePlaceholder(int signature_size,
                                       const string& payload_path,
                                       uint64_t* out_metadata_size) {
  vector<chromeos::Blob> signatures;
  signatures.push_back(chromeos::Blob(signature_size, 0));

  return PayloadSigner::AddSignatureToPayload(
      payload_path,
      signatures,
      payload_path,
      out_metadata_size);
}

static void SignGeneratedPayload(const string& payload_path,
                                 uint64_t* out_metadata_size) {
  int signature_size = GetSignatureSize(kUnittestPrivateKeyPath);
  chromeos::Blob hash;
  ASSERT_TRUE(PayloadSigner::HashPayloadForSigning(
      payload_path,
      vector<int>(1, signature_size),
      &hash));
  chromeos::Blob signature;
  ASSERT_TRUE(PayloadSigner::SignHash(hash,
                                      kUnittestPrivateKeyPath,
                                      &signature));
  ASSERT_TRUE(PayloadSigner::AddSignatureToPayload(
      payload_path,
      vector<chromeos::Blob>(1, signature),
      payload_path,
      out_metadata_size));
  EXPECT_TRUE(PayloadVerifier::VerifySignedPayload(
      payload_path,
      kUnittestPublicKeyPath));
}

static void SignGeneratedShellPayload(SignatureTest signature_test,
                                      const string& payload_path) {
  string private_key_path = kUnittestPrivateKeyPath;
  if (signature_test == kSignatureGeneratedShellBadKey) {
    ASSERT_TRUE(utils::MakeTempFile("key.XXXXXX",
                                    &private_key_path,
                                    nullptr));
  } else {
    ASSERT_TRUE(signature_test == kSignatureGeneratedShell ||
                signature_test == kSignatureGeneratedShellRotateCl1 ||
                signature_test == kSignatureGeneratedShellRotateCl2);
  }
  ScopedPathUnlinker key_unlinker(private_key_path);
  key_unlinker.set_should_remove(signature_test ==
                                 kSignatureGeneratedShellBadKey);
  // Generates a new private key that will not match the public key.
  if (signature_test == kSignatureGeneratedShellBadKey) {
    LOG(INFO) << "Generating a mismatched private key.";
    ASSERT_EQ(0, System(base::StringPrintf(
        "openssl genrsa -out %s 2048", private_key_path.c_str())));
  }
  int signature_size = GetSignatureSize(private_key_path);
  string hash_file;
  ASSERT_TRUE(utils::MakeTempFile("hash.XXXXXX", &hash_file, nullptr));
  ScopedPathUnlinker hash_unlinker(hash_file);
  string signature_size_string;
  if (signature_test == kSignatureGeneratedShellRotateCl1 ||
      signature_test == kSignatureGeneratedShellRotateCl2)
    signature_size_string = base::StringPrintf("%d:%d",
                                               signature_size, signature_size);
  else
    signature_size_string = base::StringPrintf("%d", signature_size);
  ASSERT_EQ(0,
            System(base::StringPrintf(
                "./delta_generator -in_file=%s -signature_size=%s "
                "-out_hash_file=%s",
                payload_path.c_str(),
                signature_size_string.c_str(),
                hash_file.c_str())));

  // Pad the hash
  chromeos::Blob hash;
  ASSERT_TRUE(utils::ReadFile(hash_file, &hash));
  ASSERT_TRUE(PayloadVerifier::PadRSA2048SHA256Hash(&hash));
  ASSERT_TRUE(test_utils::WriteFileVector(hash_file, hash));

  string sig_file;
  ASSERT_TRUE(utils::MakeTempFile("signature.XXXXXX", &sig_file, nullptr));
  ScopedPathUnlinker sig_unlinker(sig_file);
  ASSERT_EQ(0,
            System(base::StringPrintf(
                "openssl rsautl -raw -sign -inkey %s -in %s -out %s",
                private_key_path.c_str(),
                hash_file.c_str(),
                sig_file.c_str())));
  string sig_file2;
  ASSERT_TRUE(utils::MakeTempFile("signature.XXXXXX", &sig_file2, nullptr));
  ScopedPathUnlinker sig2_unlinker(sig_file2);
  if (signature_test == kSignatureGeneratedShellRotateCl1 ||
      signature_test == kSignatureGeneratedShellRotateCl2) {
    ASSERT_EQ(0,
              System(base::StringPrintf(
                  "openssl rsautl -raw -sign -inkey %s -in %s -out %s",
                  kUnittestPrivateKey2Path,
                  hash_file.c_str(),
                  sig_file2.c_str())));
    // Append second sig file to first path
    sig_file += ":" + sig_file2;
  }

  ASSERT_EQ(0,
            System(base::StringPrintf(
                "./delta_generator -in_file=%s -signature_file=%s "
                "-out_file=%s",
                payload_path.c_str(),
                sig_file.c_str(),
                payload_path.c_str())));
  int verify_result =
      System(base::StringPrintf(
          "./delta_generator -in_file=%s -public_key=%s -public_key_version=%d",
          payload_path.c_str(),
          signature_test == kSignatureGeneratedShellRotateCl2 ?
          kUnittestPublicKey2Path : kUnittestPublicKeyPath,
          signature_test == kSignatureGeneratedShellRotateCl2 ? 2 : 1));
  if (signature_test == kSignatureGeneratedShellBadKey) {
    ASSERT_NE(0, verify_result);
  } else {
    ASSERT_EQ(0, verify_result);
  }
}

static void GenerateDeltaFile(bool full_kernel,
                              bool full_rootfs,
                              bool noop,
                              ssize_t chunk_size,
                              SignatureTest signature_test,
                              DeltaState *state,
                              uint32_t minor_version) {
  EXPECT_TRUE(utils::MakeTempFile("a_img.XXXXXX", &state->a_img, nullptr));
  EXPECT_TRUE(utils::MakeTempFile("b_img.XXXXXX", &state->b_img, nullptr));

  // result_img is used in minor version 2. Instead of applying the update
  // in-place on A, we apply it to a new image, result_img.
  EXPECT_TRUE(
      utils::MakeTempFile("result_img.XXXXXX", &state->result_img, nullptr));
  test_utils::CreateExtImageAtPath(state->a_img, nullptr);

  state->image_size = utils::FileSize(state->a_img);

  // Create ImageInfo A & B
  ImageInfo old_image_info;
  ImageInfo new_image_info;

  if (!full_rootfs) {
    old_image_info.set_channel("src-channel");
    old_image_info.set_board("src-board");
    old_image_info.set_version("src-version");
    old_image_info.set_key("src-key");
    old_image_info.set_build_channel("src-build-channel");
    old_image_info.set_build_version("src-build-version");
  }

  new_image_info.set_channel("test-channel");
  new_image_info.set_board("test-board");
  new_image_info.set_version("test-version");
  new_image_info.set_key("test-key");
  new_image_info.set_build_channel("test-build-channel");
  new_image_info.set_build_version("test-build-version");

  // Make some changes to the A image.
  {
    string a_mnt;
    ScopedLoopMounter b_mounter(state->a_img, &a_mnt, 0);

    chromeos::Blob hardtocompress;
    while (hardtocompress.size() < 3 * kBlockSize) {
      hardtocompress.insert(hardtocompress.end(),
                            std::begin(kRandomString), std::end(kRandomString));
    }
    EXPECT_TRUE(utils::WriteFile(base::StringPrintf("%s/hardtocompress",
                                                    a_mnt.c_str()).c_str(),
                                 hardtocompress.data(),
                                 hardtocompress.size()));

    chromeos::Blob zeros(16 * 1024, 0);
    EXPECT_EQ(zeros.size(),
              base::WriteFile(base::FilePath(base::StringPrintf(
                                  "%s/move-to-sparse", a_mnt.c_str())),
                              reinterpret_cast<const char*>(zeros.data()),
                              zeros.size()));

    EXPECT_TRUE(
        WriteSparseFile(base::StringPrintf("%s/move-from-sparse",
                                           a_mnt.c_str()), 16 * 1024));

    EXPECT_EQ(0,
              System(base::StringPrintf("dd if=/dev/zero of=%s/move-semi-sparse"
                                        " bs=1 seek=4096 count=1 status=none",
                                        a_mnt.c_str()).c_str()));

    // Write 1 MiB of 0xff to try to catch the case where writing a bsdiff
    // patch fails to zero out the final block.
    chromeos::Blob ones(1024 * 1024, 0xff);
    EXPECT_TRUE(utils::WriteFile(base::StringPrintf("%s/ones",
                                                    a_mnt.c_str()).c_str(),
                                 ones.data(),
                                 ones.size()));
  }

  if (noop) {
    EXPECT_TRUE(base::CopyFile(base::FilePath(state->a_img),
                               base::FilePath(state->b_img)));
    old_image_info = new_image_info;
  } else {
    if (minor_version == kSourceMinorPayloadVersion) {
      // Create a result image with image_size bytes of garbage.
      chromeos::Blob ones(state->image_size, 0xff);
      EXPECT_TRUE(utils::WriteFile(state->result_img.c_str(),
                                   ones.data(),
                                   ones.size()));
      EXPECT_EQ(utils::FileSize(state->a_img),
                utils::FileSize(state->result_img));
    }

    test_utils::CreateExtImageAtPath(state->b_img, nullptr);

    // Make some changes to the B image.
    string b_mnt;
    ScopedLoopMounter b_mounter(state->b_img, &b_mnt, 0);

    EXPECT_EQ(0, System(base::StringPrintf("cp %s/hello %s/hello2",
                                           b_mnt.c_str(),
                                           b_mnt.c_str()).c_str()));
    EXPECT_EQ(0, System(base::StringPrintf("rm %s/hello",
                                           b_mnt.c_str()).c_str()));
    EXPECT_EQ(0, System(base::StringPrintf("mv %s/hello2 %s/hello",
                                           b_mnt.c_str(),
                                           b_mnt.c_str()).c_str()));
    EXPECT_EQ(0, System(base::StringPrintf("echo foo > %s/foo",
                                           b_mnt.c_str()).c_str()));
    EXPECT_EQ(0, System(base::StringPrintf("touch %s/emptyfile",
                                           b_mnt.c_str()).c_str()));
    EXPECT_TRUE(WriteSparseFile(base::StringPrintf("%s/fullsparse",
                                                   b_mnt.c_str()),
                                                   1024 * 1024));

    EXPECT_TRUE(
        WriteSparseFile(base::StringPrintf("%s/move-to-sparse", b_mnt.c_str()),
                        16 * 1024));

    chromeos::Blob zeros(16 * 1024, 0);
    EXPECT_EQ(zeros.size(),
              base::WriteFile(base::FilePath(base::StringPrintf(
                                  "%s/move-from-sparse", b_mnt.c_str())),
                              reinterpret_cast<const char*>(zeros.data()),
                              zeros.size()));

    EXPECT_EQ(0, System(base::StringPrintf("dd if=/dev/zero "
                                           "of=%s/move-semi-sparse "
                                           "bs=1 seek=4096 count=1 status=none",
                                           b_mnt.c_str()).c_str()));

    EXPECT_EQ(0, System(base::StringPrintf("dd if=/dev/zero "
                                           "of=%s/partsparse bs=1 "
                                           "seek=4096 count=1 status=none",
                                           b_mnt.c_str()).c_str()));
    EXPECT_EQ(0, System(base::StringPrintf("cp %s/srchardlink0 %s/tmp && "
                                           "mv %s/tmp %s/srchardlink1",
                                           b_mnt.c_str(),
                                           b_mnt.c_str(),
                                           b_mnt.c_str(),
                                           b_mnt.c_str()).c_str()));
    EXPECT_EQ(0, System(
        base::StringPrintf("rm %s/boguslink && echo foobar > %s/boguslink",
                           b_mnt.c_str(), b_mnt.c_str()).c_str()));

    chromeos::Blob hardtocompress;
    while (hardtocompress.size() < 3 * kBlockSize) {
      hardtocompress.insert(hardtocompress.end(),
                            std::begin(kRandomString), std::end(kRandomString));
    }
    EXPECT_TRUE(utils::WriteFile(base::StringPrintf("%s/hardtocompress",
                                              b_mnt.c_str()).c_str(),
                                 hardtocompress.data(),
                                 hardtocompress.size()));
  }

  string old_kernel;
  EXPECT_TRUE(utils::MakeTempFile("old_kernel.XXXXXX",
                                  &state->old_kernel,
                                  nullptr));

  string new_kernel;
  EXPECT_TRUE(utils::MakeTempFile("new_kernel.XXXXXX",
                                  &state->new_kernel,
                                  nullptr));

  string result_kernel;
  EXPECT_TRUE(utils::MakeTempFile("result_kernel.XXXXXX",
                                  &state->result_kernel,
                                  nullptr));

  state->kernel_size = kDefaultKernelSize;
  state->old_kernel_data.resize(kDefaultKernelSize);
  state->new_kernel_data.resize(state->old_kernel_data.size());
  state->result_kernel_data.resize(state->old_kernel_data.size());
  test_utils::FillWithData(&state->old_kernel_data);
  test_utils::FillWithData(&state->new_kernel_data);
  test_utils::FillWithData(&state->result_kernel_data);

  // change the new kernel data
  std::copy(std::begin(kNewData), std::end(kNewData),
            state->new_kernel_data.begin());

  if (noop) {
    state->old_kernel_data = state->new_kernel_data;
  }

  // Write kernels to disk
  EXPECT_TRUE(utils::WriteFile(state->old_kernel.c_str(),
                               state->old_kernel_data.data(),
                               state->old_kernel_data.size()));
  EXPECT_TRUE(utils::WriteFile(state->new_kernel.c_str(),
                               state->new_kernel_data.data(),
                               state->new_kernel_data.size()));
  EXPECT_TRUE(utils::WriteFile(state->result_kernel.c_str(),
                               state->result_kernel_data.data(),
                               state->result_kernel_data.size()));

  EXPECT_TRUE(utils::MakeTempFile("delta.XXXXXX",
                                  &state->delta_path,
                                  nullptr));
  LOG(INFO) << "delta path: " << state->delta_path;
  {
    const string private_key =
        signature_test == kSignatureGenerator ? kUnittestPrivateKeyPath : "";

    PayloadGenerationConfig payload_config;
    payload_config.is_delta = !full_rootfs;
    payload_config.hard_chunk_size = chunk_size;
    payload_config.rootfs_partition_size = kRootFSPartitionSize;
    payload_config.major_version = kChromeOSMajorPayloadVersion;
    payload_config.minor_version = minor_version;
    if (!full_rootfs) {
      payload_config.source.partitions.emplace_back(kLegacyPartitionNameRoot);
      payload_config.source.partitions.emplace_back(kLegacyPartitionNameKernel);
      payload_config.source.partitions.front().path = state->a_img;
      if (!full_kernel)
        payload_config.source.partitions.back().path = state->old_kernel;
      payload_config.source.image_info = old_image_info;
      EXPECT_TRUE(payload_config.source.LoadImageSize());
      for (PartitionConfig& part : payload_config.source.partitions)
        EXPECT_TRUE(part.OpenFilesystem());
    } else {
      if (payload_config.hard_chunk_size == -1)
        // Use 1 MiB chunk size for the full unittests.
        payload_config.hard_chunk_size = 1024 * 1024;
    }
    payload_config.target.partitions.emplace_back(kLegacyPartitionNameRoot);
    payload_config.target.partitions.back().path = state->b_img;
    payload_config.target.partitions.emplace_back(kLegacyPartitionNameKernel);
    payload_config.target.partitions.back().path = state->new_kernel;
    payload_config.target.image_info = new_image_info;
    EXPECT_TRUE(payload_config.target.LoadImageSize());
    for (PartitionConfig& part : payload_config.target.partitions)
      EXPECT_TRUE(part.OpenFilesystem());

    EXPECT_TRUE(payload_config.Validate());
    EXPECT_TRUE(
        GenerateUpdatePayloadFile(
            payload_config,
            state->delta_path,
            private_key,
            &state->metadata_size));
  }
  // Extend the "partitions" holding the file system a bit.
  EXPECT_EQ(0, HANDLE_EINTR(truncate(state->a_img.c_str(),
                                     state->image_size + 1024 * 1024)));
  EXPECT_EQ(state->image_size + 1024 * 1024, utils::FileSize(state->a_img));
  EXPECT_EQ(0, HANDLE_EINTR(truncate(state->b_img.c_str(),
                                     state->image_size + 1024 * 1024)));
  EXPECT_EQ(state->image_size + 1024 * 1024, utils::FileSize(state->b_img));

  if (signature_test == kSignatureGeneratedPlaceholder ||
      signature_test == kSignatureGeneratedPlaceholderMismatch) {
    int signature_size = GetSignatureSize(kUnittestPrivateKeyPath);
    LOG(INFO) << "Inserting placeholder signature.";
    ASSERT_TRUE(InsertSignaturePlaceholder(signature_size, state->delta_path,
                                           &state->metadata_size));

    if (signature_test == kSignatureGeneratedPlaceholderMismatch) {
      signature_size -= 1;
      LOG(INFO) << "Inserting mismatched placeholder signature.";
      ASSERT_FALSE(InsertSignaturePlaceholder(signature_size, state->delta_path,
                                              &state->metadata_size));
      return;
    }
  }

  if (signature_test == kSignatureGenerated ||
      signature_test == kSignatureGeneratedPlaceholder ||
      signature_test == kSignatureGeneratedPlaceholderMismatch) {
    // Generate the signed payload and update the metadata size in state to
    // reflect the new size after adding the signature operation to the
    // manifest.
    LOG(INFO) << "Signing payload.";
    SignGeneratedPayload(state->delta_path, &state->metadata_size);
  } else if (signature_test == kSignatureGeneratedShell ||
             signature_test == kSignatureGeneratedShellBadKey ||
             signature_test == kSignatureGeneratedShellRotateCl1 ||
             signature_test == kSignatureGeneratedShellRotateCl2) {
    SignGeneratedShellPayload(signature_test, state->delta_path);
  }
}

static void ApplyDeltaFile(bool full_kernel, bool full_rootfs, bool noop,
                           SignatureTest signature_test, DeltaState* state,
                           bool hash_checks_mandatory,
                           OperationHashTest op_hash_test,
                           DeltaPerformer** performer,
                           uint32_t minor_version) {
  // Check the metadata.
  {
    DeltaArchiveManifest manifest;
    EXPECT_TRUE(PayloadVerifier::LoadPayload(state->delta_path,
                                             &state->delta,
                                             &manifest,
                                             &state->metadata_size));
    LOG(INFO) << "Metadata size: " << state->metadata_size;



    if (signature_test == kSignatureNone) {
      EXPECT_FALSE(manifest.has_signatures_offset());
      EXPECT_FALSE(manifest.has_signatures_size());
    } else {
      EXPECT_TRUE(manifest.has_signatures_offset());
      EXPECT_TRUE(manifest.has_signatures_size());
      Signatures sigs_message;
      EXPECT_TRUE(sigs_message.ParseFromArray(
          &state->delta[state->metadata_size + manifest.signatures_offset()],
          manifest.signatures_size()));
      if (signature_test == kSignatureGeneratedShellRotateCl1 ||
          signature_test == kSignatureGeneratedShellRotateCl2)
        EXPECT_EQ(2, sigs_message.signatures_size());
      else
        EXPECT_EQ(1, sigs_message.signatures_size());
      const Signatures_Signature& signature = sigs_message.signatures(0);
      EXPECT_EQ(1, signature.version());

      uint64_t expected_sig_data_length = 0;
      vector<string> key_paths{kUnittestPrivateKeyPath};
      if (signature_test == kSignatureGeneratedShellRotateCl1 ||
          signature_test == kSignatureGeneratedShellRotateCl2) {
        key_paths.push_back(kUnittestPrivateKey2Path);
      }
      EXPECT_TRUE(PayloadSigner::SignatureBlobLength(
          key_paths,
          &expected_sig_data_length));
      EXPECT_EQ(expected_sig_data_length, manifest.signatures_size());
      EXPECT_FALSE(signature.data().empty());
    }

    if (noop) {
      EXPECT_EQ(0, manifest.install_operations_size());
      EXPECT_EQ(1, manifest.kernel_install_operations_size());
    }

    if (full_kernel) {
      EXPECT_FALSE(manifest.has_old_kernel_info());
    } else {
      EXPECT_EQ(state->old_kernel_data.size(),
                manifest.old_kernel_info().size());
      EXPECT_FALSE(manifest.old_kernel_info().hash().empty());
    }

    EXPECT_EQ(manifest.new_image_info().channel(), "test-channel");
    EXPECT_EQ(manifest.new_image_info().board(), "test-board");
    EXPECT_EQ(manifest.new_image_info().version(), "test-version");
    EXPECT_EQ(manifest.new_image_info().key(), "test-key");
    EXPECT_EQ(manifest.new_image_info().build_channel(), "test-build-channel");
    EXPECT_EQ(manifest.new_image_info().build_version(), "test-build-version");

    if (!full_rootfs) {
      if (noop) {
        EXPECT_EQ(manifest.old_image_info().channel(), "test-channel");
        EXPECT_EQ(manifest.old_image_info().board(), "test-board");
        EXPECT_EQ(manifest.old_image_info().version(), "test-version");
        EXPECT_EQ(manifest.old_image_info().key(), "test-key");
        EXPECT_EQ(manifest.old_image_info().build_channel(),
                  "test-build-channel");
        EXPECT_EQ(manifest.old_image_info().build_version(),
                  "test-build-version");
      } else {
        EXPECT_EQ(manifest.old_image_info().channel(), "src-channel");
        EXPECT_EQ(manifest.old_image_info().board(), "src-board");
        EXPECT_EQ(manifest.old_image_info().version(), "src-version");
        EXPECT_EQ(manifest.old_image_info().key(), "src-key");
        EXPECT_EQ(manifest.old_image_info().build_channel(),
                  "src-build-channel");
        EXPECT_EQ(manifest.old_image_info().build_version(),
                  "src-build-version");
      }
    }


    if (full_rootfs) {
      EXPECT_FALSE(manifest.has_old_rootfs_info());
      EXPECT_FALSE(manifest.has_old_image_info());
      EXPECT_TRUE(manifest.has_new_image_info());
    } else {
      EXPECT_EQ(state->image_size, manifest.old_rootfs_info().size());
      EXPECT_FALSE(manifest.old_rootfs_info().hash().empty());
    }

    EXPECT_EQ(state->new_kernel_data.size(), manifest.new_kernel_info().size());
    EXPECT_EQ(state->image_size, manifest.new_rootfs_info().size());

    EXPECT_FALSE(manifest.new_kernel_info().hash().empty());
    EXPECT_FALSE(manifest.new_rootfs_info().hash().empty());
  }

  MockPrefs prefs;
  EXPECT_CALL(prefs, SetInt64(kPrefsManifestMetadataSize,
                              state->metadata_size)).WillOnce(Return(true));
  EXPECT_CALL(prefs, SetInt64(kPrefsUpdateStateNextOperation, _))
      .WillRepeatedly(Return(true));
  EXPECT_CALL(prefs, GetInt64(kPrefsUpdateStateNextOperation, _))
      .WillOnce(Return(false));
  EXPECT_CALL(prefs, SetInt64(kPrefsUpdateStateNextDataOffset, _))
      .WillRepeatedly(Return(true));
  EXPECT_CALL(prefs, SetInt64(kPrefsUpdateStateNextDataLength, _))
      .WillRepeatedly(Return(true));
  EXPECT_CALL(prefs, SetString(kPrefsUpdateStateSHA256Context, _))
      .WillRepeatedly(Return(true));
  if (op_hash_test == kValidOperationData && signature_test != kSignatureNone) {
    EXPECT_CALL(prefs, SetString(kPrefsUpdateStateSignedSHA256Context, _))
        .WillOnce(Return(true));
    EXPECT_CALL(prefs, SetString(kPrefsUpdateStateSignatureBlob, _))
        .WillOnce(Return(true));
  }

  // Update the A image in place.
  InstallPlan install_plan;
  install_plan.hash_checks_mandatory = hash_checks_mandatory;
  install_plan.metadata_size = state->metadata_size;
  install_plan.is_full_update = full_kernel && full_rootfs;
  install_plan.source_path = state->a_img.c_str();
  install_plan.kernel_source_path = state->old_kernel.c_str();

  LOG(INFO) << "Setting payload metadata size in Omaha  = "
            << state->metadata_size;
  ASSERT_TRUE(PayloadSigner::GetMetadataSignature(
      state->delta.data(),
      state->metadata_size,
      kUnittestPrivateKeyPath,
      &install_plan.metadata_signature));
  EXPECT_FALSE(install_plan.metadata_signature.empty());

  *performer = new DeltaPerformer(&prefs,
                                  &state->fake_system_state,
                                  &install_plan);
  EXPECT_TRUE(utils::FileExists(kUnittestPublicKeyPath));
  (*performer)->set_public_key_path(kUnittestPublicKeyPath);
  DeltaPerformerIntegrationTest::SetSupportedVersion(*performer, minor_version);

  EXPECT_EQ(state->image_size,
            OmahaHashCalculator::RawHashOfFile(
                state->a_img,
                state->image_size,
                &install_plan.source_rootfs_hash));
  EXPECT_TRUE(OmahaHashCalculator::RawHashOfData(
                  state->old_kernel_data,
                  &install_plan.source_kernel_hash));

  // With minor version 2, we want the target to be the new image, result_img,
  // but with version 1, we want to update A in place.
  if (minor_version == kSourceMinorPayloadVersion) {
    EXPECT_EQ(0, (*performer)->Open(state->result_img.c_str(), 0, 0));
    EXPECT_TRUE((*performer)->OpenKernel(state->result_kernel.c_str()));
  } else {
    EXPECT_EQ(0, (*performer)->Open(state->a_img.c_str(), 0, 0));
    EXPECT_TRUE((*performer)->OpenKernel(state->old_kernel.c_str()));
  }


  ErrorCode expected_error, actual_error;
  bool continue_writing;
  switch (op_hash_test) {
    case kInvalidOperationData: {
      // Muck with some random offset post the metadata size so that
      // some operation hash will result in a mismatch.
      int some_offset = state->metadata_size + 300;
      LOG(INFO) << "Tampered value at offset: " << some_offset;
      state->delta[some_offset]++;
      expected_error = ErrorCode::kDownloadOperationHashMismatch;
      continue_writing = false;
      break;
    }

    case kValidOperationData:
    default:
      // no change.
      expected_error = ErrorCode::kSuccess;
      continue_writing = true;
      break;
  }

  // Write at some number of bytes per operation. Arbitrarily chose 5.
  const size_t kBytesPerWrite = 5;
  for (size_t i = 0; i < state->delta.size(); i += kBytesPerWrite) {
    size_t count = std::min(state->delta.size() - i, kBytesPerWrite);
    bool write_succeeded = ((*performer)->Write(&state->delta[i],
                                                count,
                                                &actual_error));
    // Normally write_succeeded should be true every time and
    // actual_error should be ErrorCode::kSuccess. If so, continue the loop.
    // But if we seeded an operation hash error above, then write_succeeded
    // will be false. The failure may happen at any operation n. So, all
    // Writes until n-1 should succeed and the nth operation will fail with
    // actual_error. In this case, we should bail out of the loop because
    // we cannot proceed applying the delta.
    if (!write_succeeded) {
      LOG(INFO) << "Write failed. Checking if it failed with expected error";
      EXPECT_EQ(expected_error, actual_error);
      if (!continue_writing) {
        LOG(INFO) << "Cannot continue writing. Bailing out.";
        break;
      }
    }

    EXPECT_EQ(ErrorCode::kSuccess, actual_error);
  }

  // If we had continued all the way through, Close should succeed.
  // Otherwise, it should fail. Check appropriately.
  bool close_result = (*performer)->Close();
  if (continue_writing)
    EXPECT_EQ(0, close_result);
  else
    EXPECT_LE(0, close_result);
}

void VerifyPayloadResult(DeltaPerformer* performer,
                         DeltaState* state,
                         ErrorCode expected_result,
                         uint32_t minor_version) {
  if (!performer) {
    EXPECT_TRUE(!"Skipping payload verification since performer is null.");
    return;
  }

  int expected_times = (expected_result == ErrorCode::kSuccess) ? 1 : 0;
  EXPECT_CALL(*(state->fake_system_state.mock_payload_state()),
              DownloadComplete()).Times(expected_times);

  LOG(INFO) << "Verifying payload for expected result "
            << expected_result;
  EXPECT_EQ(expected_result, performer->VerifyPayload(
      OmahaHashCalculator::OmahaHashOfData(state->delta),
      state->delta.size()));
  LOG(INFO) << "Verified payload.";

  if (expected_result != ErrorCode::kSuccess) {
    // no need to verify new partition if VerifyPayload failed.
    return;
  }

  chromeos::Blob updated_kernel_partition;
  if (minor_version == kSourceMinorPayloadVersion) {
    CompareFilesByBlock(state->result_kernel, state->new_kernel,
                        state->kernel_size);
    CompareFilesByBlock(state->result_img, state->b_img,
                        state->image_size);
    EXPECT_TRUE(utils::ReadFile(state->result_kernel,
                                &updated_kernel_partition));
  } else {
    CompareFilesByBlock(state->old_kernel, state->new_kernel,
                        state->kernel_size);
    CompareFilesByBlock(state->a_img, state->b_img,
                        state->image_size);
    EXPECT_TRUE(utils::ReadFile(state->old_kernel, &updated_kernel_partition));
  }

  ASSERT_GE(updated_kernel_partition.size(), arraysize(kNewData));
  EXPECT_TRUE(std::equal(std::begin(kNewData), std::end(kNewData),
                         updated_kernel_partition.begin()));

  uint64_t new_kernel_size;
  chromeos::Blob new_kernel_hash;
  uint64_t new_rootfs_size;
  chromeos::Blob new_rootfs_hash;
  EXPECT_TRUE(performer->GetNewPartitionInfo(&new_kernel_size,
                                             &new_kernel_hash,
                                             &new_rootfs_size,
                                             &new_rootfs_hash));
  EXPECT_EQ(kDefaultKernelSize, new_kernel_size);
  chromeos::Blob expected_new_kernel_hash;
  EXPECT_TRUE(OmahaHashCalculator::RawHashOfData(state->new_kernel_data,
                                                 &expected_new_kernel_hash));
  EXPECT_TRUE(expected_new_kernel_hash == new_kernel_hash);
  EXPECT_EQ(state->image_size, new_rootfs_size);
  chromeos::Blob expected_new_rootfs_hash;
  EXPECT_EQ(state->image_size,
            OmahaHashCalculator::RawHashOfFile(state->b_img,
                                               state->image_size,
                                               &expected_new_rootfs_hash));
  EXPECT_TRUE(expected_new_rootfs_hash == new_rootfs_hash);
}

void VerifyPayload(DeltaPerformer* performer,
                   DeltaState* state,
                   SignatureTest signature_test,
                   uint32_t minor_version) {
  ErrorCode expected_result = ErrorCode::kSuccess;
  switch (signature_test) {
    case kSignatureNone:
      expected_result = ErrorCode::kSignedDeltaPayloadExpectedError;
      break;
    case kSignatureGeneratedShellBadKey:
      expected_result = ErrorCode::kDownloadPayloadPubKeyVerificationError;
      break;
    default: break;  // appease gcc
  }

  VerifyPayloadResult(performer, state, expected_result, minor_version);
}

void DoSmallImageTest(bool full_kernel, bool full_rootfs, bool noop,
                      ssize_t chunk_size,
                      SignatureTest signature_test,
                      bool hash_checks_mandatory, uint32_t minor_version) {
  DeltaState state;
  DeltaPerformer *performer = nullptr;
  GenerateDeltaFile(full_kernel, full_rootfs, noop, chunk_size,
                    signature_test, &state, minor_version);

  ScopedPathUnlinker a_img_unlinker(state.a_img);
  ScopedPathUnlinker b_img_unlinker(state.b_img);
  ScopedPathUnlinker new_img_unlinker(state.result_img);
  ScopedPathUnlinker delta_unlinker(state.delta_path);
  ScopedPathUnlinker old_kernel_unlinker(state.old_kernel);
  ScopedPathUnlinker new_kernel_unlinker(state.new_kernel);
  ScopedPathUnlinker result_kernel_unlinker(state.result_kernel);
  ApplyDeltaFile(full_kernel, full_rootfs, noop, signature_test,
                 &state, hash_checks_mandatory, kValidOperationData,
                 &performer, minor_version);
  VerifyPayload(performer, &state, signature_test, minor_version);
  delete performer;
}

void DoOperationHashMismatchTest(OperationHashTest op_hash_test,
                                 bool hash_checks_mandatory) {
  DeltaState state;
  uint64_t minor_version = kFullPayloadMinorVersion;
  GenerateDeltaFile(true, true, false, -1, kSignatureGenerated, &state,
                    minor_version);
  ScopedPathUnlinker a_img_unlinker(state.a_img);
  ScopedPathUnlinker b_img_unlinker(state.b_img);
  ScopedPathUnlinker delta_unlinker(state.delta_path);
  ScopedPathUnlinker old_kernel_unlinker(state.old_kernel);
  ScopedPathUnlinker new_kernel_unlinker(state.new_kernel);
  DeltaPerformer *performer = nullptr;
  ApplyDeltaFile(true, true, false, kSignatureGenerated, &state,
                 hash_checks_mandatory, op_hash_test, &performer,
                 minor_version);
  delete performer;
}


TEST(DeltaPerformerIntegrationTest, RunAsRootSmallImageTest) {
  DoSmallImageTest(false, false, false, -1, kSignatureGenerator,
                   false, kInPlaceMinorPayloadVersion);
}

TEST(DeltaPerformerIntegrationTest, RunAsRootSmallImageSignaturePlaceholderTest) {
  DoSmallImageTest(false, false, false, -1, kSignatureGeneratedPlaceholder,
                   false, kInPlaceMinorPayloadVersion);
}

TEST(DeltaPerformerIntegrationTest, RunAsRootSmallImageSignaturePlaceholderMismatchTest) {
  DeltaState state;
  GenerateDeltaFile(false, false, false, -1,
                    kSignatureGeneratedPlaceholderMismatch, &state,
                    kInPlaceMinorPayloadVersion);
}

TEST(DeltaPerformerIntegrationTest, RunAsRootSmallImageChunksTest) {
  DoSmallImageTest(false, false, false, kBlockSize, kSignatureGenerator,
                   false, kInPlaceMinorPayloadVersion);
}

TEST(DeltaPerformerIntegrationTest, RunAsRootFullKernelSmallImageTest) {
  DoSmallImageTest(true, false, false, -1, kSignatureGenerator,
                   false, kInPlaceMinorPayloadVersion);
}

TEST(DeltaPerformerIntegrationTest, RunAsRootFullSmallImageTest) {
  DoSmallImageTest(true, true, false, -1, kSignatureGenerator,
                   true, kFullPayloadMinorVersion);
}

TEST(DeltaPerformerIntegrationTest, RunAsRootNoopSmallImageTest) {
  DoSmallImageTest(false, false, true, -1, kSignatureGenerator,
                   false, kInPlaceMinorPayloadVersion);
}

TEST(DeltaPerformerIntegrationTest, RunAsRootSmallImageSignNoneTest) {
  DoSmallImageTest(false, false, false, -1, kSignatureNone,
                   false, kInPlaceMinorPayloadVersion);
}

TEST(DeltaPerformerIntegrationTest, RunAsRootSmallImageSignGeneratedTest) {
  DoSmallImageTest(false, false, false, -1, kSignatureGenerated,
                   true, kInPlaceMinorPayloadVersion);
}

TEST(DeltaPerformerIntegrationTest, RunAsRootSmallImageSignGeneratedShellTest) {
  DoSmallImageTest(false, false, false, -1, kSignatureGeneratedShell,
                   false, kInPlaceMinorPayloadVersion);
}

TEST(DeltaPerformerIntegrationTest, RunAsRootSmallImageSignGeneratedShellBadKeyTest) {
  DoSmallImageTest(false, false, false, -1, kSignatureGeneratedShellBadKey,
                   false, kInPlaceMinorPayloadVersion);
}

TEST(DeltaPerformerIntegrationTest, RunAsRootSmallImageSignGeneratedShellRotateCl1Test) {
  DoSmallImageTest(false, false, false, -1, kSignatureGeneratedShellRotateCl1,
                   false, kInPlaceMinorPayloadVersion);
}

TEST(DeltaPerformerIntegrationTest, RunAsRootSmallImageSignGeneratedShellRotateCl2Test) {
  DoSmallImageTest(false, false, false, -1, kSignatureGeneratedShellRotateCl2,
                   false, kInPlaceMinorPayloadVersion);
}

TEST(DeltaPerformerIntegrationTest, RunAsRootSmallImageSourceOpsTest) {
  DoSmallImageTest(false, false, false, -1, kSignatureGenerator,
                   false, kSourceMinorPayloadVersion);
}

TEST(DeltaPerformerIntegrationTest, RunAsRootMandatoryOperationHashMismatchTest) {
  DoOperationHashMismatchTest(kInvalidOperationData, true);
}

}  // namespace chromeos_update_engine
