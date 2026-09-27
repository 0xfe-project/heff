-- | What differs between an expectation and a value, for a test's report:
-- | both, cut to where they differ, and the difference, `[-expected-]` and
-- | `{+got+}` (in colour too, on a terminal).
module Hg.Support.Diff
  ( mismatch
  , useColor
  ) where

import Effect (Effect)

foreign import mismatchImpl :: Boolean -> String -> String -> { expected :: String, got :: String, diff :: String }

foreign import useColorImpl :: Effect Boolean

mismatch :: Boolean -> String -> String -> { expected :: String, got :: String, diff :: String }
mismatch = mismatchImpl

-- | stdout is a terminal, and `NO_COLOR` is not set
useColor :: Effect Boolean
useColor = useColorImpl
