/* FmFileProvider.java -- serves local files to other apps as content:// URIs.
**
** URI form: content://io.github.mmc.filemanager.files/<encoded absolute path>
** Read-only; access is granted per intent with FLAG_GRANT_READ_URI_PERMISSION
** (the provider itself is not exported).
*/
package io.github.mmc.filemanager;

import android.content.ContentProvider;
import android.content.ContentValues;
import android.database.Cursor;
import android.database.MatrixCursor;
import android.net.Uri;
import android.os.ParcelFileDescriptor;
import android.provider.OpenableColumns;
import android.webkit.MimeTypeMap;

import java.io.File;
import java.io.FileNotFoundException;
import java.util.Locale;

public class FmFileProvider extends ContentProvider {

    static final String AUTHORITY = "io.github.mmc.filemanager.files";

    static Uri uriFor(String path) {
        return new Uri.Builder().scheme("content").authority(AUTHORITY)
                .encodedPath(Uri.encode(path, "/")).build();
    }

    private static File fileOf(Uri uri) throws FileNotFoundException {
        String p = uri.getPath();
        if (p == null) throw new FileNotFoundException("no path");
        File f = new File(p);
        if (!f.isFile()) throw new FileNotFoundException(p);
        return f;
    }

    @Override
    public boolean onCreate() {
        return true;
    }

    @Override
    public ParcelFileDescriptor openFile(Uri uri, String mode) throws FileNotFoundException {
        if (mode != null && mode.contains("w")) throw new FileNotFoundException("read-only");
        return ParcelFileDescriptor.open(fileOf(uri), ParcelFileDescriptor.MODE_READ_ONLY);
    }

    @Override
    public String getType(Uri uri) {
        String p = uri.getPath();
        if (p != null) {
            int dot = p.lastIndexOf('.');
            if (dot >= 0) {
                String m = MimeTypeMap.getSingleton()
                        .getMimeTypeFromExtension(p.substring(dot + 1).toLowerCase(Locale.ROOT));
                if (m != null) return m;
            }
        }
        return "application/octet-stream";
    }

    @Override
    public Cursor query(Uri uri, String[] projection, String selection, String[] args, String sort) {
        File f;
        try {
            f = fileOf(uri);
        } catch (FileNotFoundException e) {
            return null;
        }
        String[] cols = projection != null ? projection
                : new String[] { OpenableColumns.DISPLAY_NAME, OpenableColumns.SIZE };
        MatrixCursor c = new MatrixCursor(cols, 1);
        Object[] row = new Object[cols.length];
        for (int i = 0; i < cols.length; i++) {
            if (OpenableColumns.DISPLAY_NAME.equals(cols[i])) row[i] = f.getName();
            else if (OpenableColumns.SIZE.equals(cols[i])) row[i] = f.length();
        }
        c.addRow(row);
        return c;
    }

    @Override
    public Uri insert(Uri uri, ContentValues values) {
        return null;
    }

    @Override
    public int delete(Uri uri, String selection, String[] args) {
        return 0;
    }

    @Override
    public int update(Uri uri, ContentValues values, String selection, String[] args) {
        return 0;
    }
}
