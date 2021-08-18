# typed: true

# This test case exposes problems reported in https://github.com/sorbet/sorbet/issues/4426

module M

  # The class generated by describe shouldn't use `M` as a super-class, as
  # it's a module.
  describe "describe" do

    it "adds a method" do
    end

  end

end

class C

  def test_method
  end

  # The class extracted here will have `C` as a super-class, allowing
  # `test_method` to be used in `it` blocks.
  describe "describe" do

    it "adds a method" do
      test_method
    end

  end
end
