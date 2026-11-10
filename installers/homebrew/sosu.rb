# Formula of a Homebrew tap for sosuc, filled by installers/fill-manifests.sh for each release.
# Not published before 1.0: it goes into a repository Clement-Bonet/homebrew-sosu, then
#   brew install clement-bonet/sosu/sosu
class Sosu < Formula
  desc "Compiler of the Sosu programming language"
  homepage "https://github.com/Clement-Bonet/Sosu"
  version "@VERSION@"
  license any_of: ["MIT", "Apache-2.0"]

  on_macos do
    url "https://github.com/Clement-Bonet/Sosu/releases/download/v@VERSION@/sosu-@VERSION@-macos-universal.tar.gz"
    sha256 "@SHA256_MACOS@"
  end

  on_linux do
    on_intel do
      url "https://github.com/Clement-Bonet/Sosu/releases/download/v@VERSION@/sosu-@VERSION@-linux-x64.tar.gz"
      sha256 "@SHA256_LINUX_X64@"
    end
    on_arm do
      url "https://github.com/Clement-Bonet/Sosu/releases/download/v@VERSION@/sosu-@VERSION@-linux-arm64.tar.gz"
      sha256 "@SHA256_LINUX_ARM64@"
    end
  end

  # sosuc finds its library in ../lib next to the program it was run as: the archive stays whole
  # in libexec, and bin/sosuc is a script that runs libexec/bin/sosuc by its full path.
  def install
    libexec.install "bin", "lib", "packages"
    bin.write_exec_script libexec/"bin/sosuc"
  end

  def caveats
    <<~EOS
      The packages of the release are in #{opt_libexec}/packages.
    EOS
  end

  test do
    assert_match version.to_s, shell_output("#{bin}/sosuc --version")
  end
end
