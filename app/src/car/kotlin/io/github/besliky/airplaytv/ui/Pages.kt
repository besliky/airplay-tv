package io.github.besliky.airplaytv.ui

import android.content.Context
import android.content.Intent
import io.github.besliky.airplaytv.car.CarActivity

/** The car app shows every [Page] in its one shell, [CarActivity]. */
object Pages {
    fun intent(context: Context, page: Page): Intent =
        Intent(context, CarActivity::class.java).putExtra(CarActivity.EXTRA_PAGE, page.name)
}
